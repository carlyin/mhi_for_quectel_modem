#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/poll.h>
#include <sys/time.h>

static const char * firehose_get_time(void);
#define dbg_time(fmt, args...) do {printf("[%s] " fmt, firehose_get_time(), ##args); } while(0)
#define error_return()  do {dbg_time("%s %s %d fail\n", __FILE__, __func__, __LINE__); return __LINE__; } while(0)

struct fh_configure_cmd {
    const char *type;
    const char *MemoryName;
    uint32_t Verbose;
    uint32_t AlwaysValidate;
    uint32_t MaxDigestTableSizeInBytes;
    uint32_t MaxPayloadSizeToTargetInBytes;
    uint32_t MaxPayloadSizeFromTargetInBytes ; 			//2048
    uint32_t MaxPayloadSizeToTargetInByteSupported;		//16k
    uint32_t ZlpAwareHost;
    uint32_t SkipStorageInit;
};

struct fh_erase_cmd {
    const char *type;
    uint32_t PAGES_PER_BLOCK;
    uint32_t SECTOR_SIZE_IN_BYTES;
    char label[32];
    uint32_t last_sector;
    uint32_t num_partition_sectors;
    uint32_t physical_partition_number;
    uint32_t start_sector;
};

struct fh_program_cmd {
    const char *type;
    char *filename;
    uint32_t filesz;
    uint32_t PAGES_PER_BLOCK;
    uint32_t SECTOR_SIZE_IN_BYTES;
    char label[32];
    uint32_t last_sector;
    uint32_t num_partition_sectors;
    uint32_t physical_partition_number;
    uint32_t start_sector;
};

struct fh_response_cmd {
    const char *type;
    const char *value;
    uint32_t rawmode;
    uint32_t MaxPayloadSizeToTargetInBytes;
};

struct fh_log_cmd {
    const char *type;
};

struct fh_cmd_header {
    const char *type;
};

struct fh_vendor_defines {
    const char *type; // "vendor"
    char buffer[256];
};

struct fh_cmd {
    union {
        struct fh_cmd_header cmd;
        struct fh_configure_cmd cfg;
        struct fh_erase_cmd erase;
        struct fh_program_cmd program;
        struct fh_response_cmd response;
        struct fh_log_cmd log;
        struct fh_vendor_defines vdef;
    };
    int part_upgrade;
};

struct fh_data {
    const char *firehose_dir;
    int edl_fd;
    unsigned MaxPayloadSizeToTargetInBytes;
    unsigned fh_cmd_count;
    unsigned ZlpAwareHost;
    struct fh_cmd fh_cmd_table[128]; //AG525 have more than 64 partition
    
    unsigned xml_size;
    char xml_buf[1024];
};

static unsigned char edl_cmd[] = {0x4b, 0x65, 0x01, 0x00, 0x54, 0x0f, 0x7e};
static unsigned char rx_buf[2048];

static long long all_bytes_to_transfer = 0;    //need transfered
static long long transfer_bytes = 0;        //transfered bytes;
static int transfer_percent = 0;
static int fh_ignore_read_timeout = 0;

static void show_progress()
{
    if (all_bytes_to_transfer)
        transfer_percent = (transfer_bytes * 100) / all_bytes_to_transfer;
    dbg_time("upgrade progress %d%% %lld/%lld\n", transfer_percent, transfer_bytes, all_bytes_to_transfer);
}

static void update_transfer_bytes(long long bytes_cur)
{
	transfer_bytes += bytes_cur;
}

static void set_transfer_allbytes(long long bytes)
{
	transfer_percent = 0;
	transfer_bytes = 0;
	all_bytes_to_transfer = bytes;
}

static int qfile_find_xmlfile(const char *dir, const char *prefix, char** xmlfile) {
    DIR *pdir;
    struct dirent* ent = NULL;
    pdir = opendir(dir);

    dbg_time("dir=%s\n", dir);
    if(pdir)
    {
        while((ent = readdir(pdir)) != NULL)
        {
            if(!strncmp(ent->d_name, prefix, strlen(prefix)))
            {
                dbg_time("d_name=%s\n", ent->d_name);
                *xmlfile = strdup(ent->d_name);
                closedir(pdir);
                return 0;
            }  
        }
    }

    closedir(pdir);
    return 1;
}

static const char * fh_xml_get_value(const char *xml_line, const char *key) {
    static char value[64];

    char *pchar = strstr(xml_line, key);
    char *pend;

    if (!pchar) {
        dbg_time("%s: no key %s in %s\n", __func__, key, xml_line);
        return NULL;
    }

    pchar += strlen(key);
    if (pchar[0] != '=' && pchar[1] != '"') {
        dbg_time("%s: no start %s in %s\n", __func__, "=\"", xml_line);
        return NULL;
    }

    pchar += strlen("=\"");
    pend = strstr(pchar, "\"");
    if (!pend) {
        dbg_time("%s: no end %s in %s\n", __func__, "\"", xml_line);
        return NULL;
    }
    
    strncpy(value, pchar, pend - pchar);
    value[pend - pchar] = '\0';

    //dbg_time("%s=%s\n", key, value);

    return value;
}

static const char * firehose_get_time(void) {
    static char time_buf[50];
    struct timeval  tv;
    static int s_start_msec = -1;
    int now_msec, cost_msec;

    gettimeofday (&tv, NULL);
    now_msec = tv.tv_sec * 1000;
    now_msec += (tv.tv_usec + 500) / 1000;

    if (s_start_msec == -1) {
        s_start_msec = now_msec;
    }

    cost_msec = now_msec - s_start_msec;

    sprintf(time_buf, "%03d.%03d", cost_msec/1000, cost_msec%1000);
    return time_buf;
}

static int fh_write(int fd, void *pbuf, size_t size, unsigned timeout_msec) {
	int ret;
	struct pollfd pollfds[] = {{fd, POLLOUT, 0}};

	ret = poll(pollfds, 1, timeout_msec);
	if (ret <= 0) {
		dbg_time("fail to poll write ret=%d, errno: %d (%s)\n", ret, errno, strerror(errno));
		return ret;
	}

	ret = write(fd, pbuf, size);
	if (ret <= 0) {
		dbg_time("fail to write ret=%d, errno: %d (%s)\n", ret, errno, strerror(errno));
	}

	return ret;
}

static int fh_read(int fd, void *pbuf, size_t size, unsigned timeout_msec) {
	int ret;
	struct pollfd pollfds[] = {{fd, POLLIN, 0}};

	ret = poll(pollfds, 1, timeout_msec);
	if (ret <= 0) {
	 	if (ret == 0 && fh_ignore_read_timeout)
			return ret;
		dbg_time("fail to poll read ret=%d, errno: %d (%s)\n", ret, errno, strerror(errno));
		return ret;
	}

	ret = read(fd, pbuf, size);
	if (ret <= 0) {
		dbg_time("fail to read ret=%d, errno: %d (%s)\n", ret, errno, strerror(errno));
	}

	return ret;
}

static int fh_parse_xml_line(const char *xml_line, struct fh_cmd *fh_cmd) {
    const char *pchar = NULL;
    char *pret;
    
    memset(fh_cmd, 0, sizeof( struct fh_cmd));
    if (strstr(xml_line, "vendor=\"quectel\"")) {
        fh_cmd->vdef.type = "vendor";
        snprintf(fh_cmd->vdef.buffer, sizeof(fh_cmd->vdef.buffer), "%s", xml_line);
        return 0;
    }
    else if (!strncmp(xml_line, "<erase ", strlen("<erase "))) {
        fh_cmd->erase.type = "erase";
        if ((pchar = fh_xml_get_value(xml_line, "PAGES_PER_BLOCK")))
            fh_cmd->erase.PAGES_PER_BLOCK = atoi(pchar);
        if ((pchar = fh_xml_get_value(xml_line, "SECTOR_SIZE_IN_BYTES")))
            fh_cmd->erase.SECTOR_SIZE_IN_BYTES = atoi(pchar);
        if (strstr(xml_line, "label") && strstr(xml_line, "last_sector")) {
            if ((pchar = fh_xml_get_value(xml_line, "label")))
                strcpy(fh_cmd->erase.label, pchar);
            if ((pchar = fh_xml_get_value(xml_line, "last_sector")))
                fh_cmd->erase.last_sector = atoi(pchar);
        }
        if ((pchar = fh_xml_get_value(xml_line, "num_partition_sectors")))
            fh_cmd->erase.num_partition_sectors = strtoul(pchar, &pret, 10);
        if ((pchar = fh_xml_get_value(xml_line, "physical_partition_number")))
            fh_cmd->erase.physical_partition_number = atoi(pchar);
        if ((pchar = fh_xml_get_value(xml_line, "start_sector")))
            fh_cmd->erase.start_sector = atoi(pchar);
        return 0;
    }
    else if (!strncmp(xml_line, "<program ", strlen("<program "))) {
        fh_cmd->program.type = "program";
        if ((pchar = fh_xml_get_value(xml_line, "filename")))
        {
            fh_cmd->program.filename = strdup(pchar);                          
            if(fh_cmd->program.filename[0] == '\0')
            {//some fw version have blank program line, ignore it.
                return -1;
            }
        }
        if ((pchar = fh_xml_get_value(xml_line, "PAGES_PER_BLOCK")))
            fh_cmd->program.PAGES_PER_BLOCK = atoi(pchar);
        if ((pchar = fh_xml_get_value(xml_line, "SECTOR_SIZE_IN_BYTES")))
            fh_cmd->program.SECTOR_SIZE_IN_BYTES = atoi(pchar);
        if (strstr(xml_line, "label") && strstr(xml_line, "last_sector")) {
            if ((pchar = fh_xml_get_value(xml_line, "label")))
                strcpy(fh_cmd->program.label, pchar);
            if ((pchar = fh_xml_get_value(xml_line, "last_sector")))
                fh_cmd->program.last_sector = atoi(pchar);
        }
        if ((pchar = fh_xml_get_value(xml_line, "num_partition_sectors")))
            fh_cmd->program.num_partition_sectors = atoi(pchar);
        if ((pchar = fh_xml_get_value(xml_line, "physical_partition_number")))
            fh_cmd->program.physical_partition_number = atoi(pchar);
        if ((pchar = fh_xml_get_value(xml_line, "start_sector")))
            fh_cmd->program.start_sector = atoi(pchar);
        return 0;
    }
    else if (!strncmp(xml_line, "<response ", strlen("<response "))) {
        fh_cmd->response.type = "response";
        pchar = fh_xml_get_value(xml_line, "value");
        if (pchar) {
            if (!strcmp(pchar, "ACK"))
                fh_cmd->response.value =  "ACK";
            else if(!strcmp(pchar, "NAK"))
                fh_cmd->response.value =  "NAK";
            else
                 fh_cmd->response.value =  "OTHER";               
        }
        if (strstr(xml_line, "rawmode")) {
            pchar = fh_xml_get_value(xml_line, "rawmode");
            if (pchar) {
                fh_cmd->response.rawmode = !strcmp(pchar, "true");
            }
        }
        else if (strstr(xml_line, "MaxPayloadSizeToTargetInBytes")) {
            pchar = fh_xml_get_value(xml_line, "MaxPayloadSizeToTargetInBytes");
            if (pchar) {
                fh_cmd->response.MaxPayloadSizeToTargetInBytes = atoi(pchar);
            }
        }
        return 0;
    }
    else if (!strncmp(xml_line, "<log ", strlen("<log "))) {
        fh_cmd->program.type = "log";
        return 0;
    }

    error_return();
}

static int fh_parse_xml_file(struct fh_data *fh_data, const char *xml_file) {
    FILE *fp = fopen(xml_file, "rb");
    
    if (fp == NULL) {
        dbg_time("%s fail to fopen(%s), errno: %d (%s)\n", __func__, xml_file, errno, strerror(errno));
        error_return();
    }

    while (fgets(fh_data->xml_buf, fh_data->xml_size, fp)) {
        char *xml_line = strstr(fh_data->xml_buf, "<");
		
		if (xml_line && strstr(xml_line, "<!--")) {
			if (strstr(xml_line, "-->")) {
				if (strstr(xml_line, "/>") < strstr(xml_line, "<!--"))
					goto __fh_parse_xml_line;

				continue;
			} else {
				do {
					fgets(fh_data->xml_buf, fh_data->xml_size, fp);
					xml_line = fh_data->xml_buf;
				} while(!strstr(xml_line, "-->") && strstr(xml_line, "<!--"));

				continue;
			}
		}

__fh_parse_xml_line:		
        if (xml_line &&
                    (strstr(xml_line, "<erase ") || 
                    strstr(xml_line, "<program ") || 
                    strstr(xml_line, "vendor=\"quectel\""))) {
            if (!fh_parse_xml_line(xml_line, &fh_data->fh_cmd_table[fh_data->fh_cmd_count]))
                fh_data->fh_cmd_count++;
        }
    }

    fclose(fp);

    return 0;
}

static int fh_fixup_program_cmd(struct fh_data *fh_data, struct fh_cmd *fh_cmd, long* filesize_out) {
    char full_path[256];
    char *unix_filename = strdup(fh_cmd->program.filename);
    char *ptmp;
    FILE *fp;
    long filesize = 0;

    while((ptmp = strchr(unix_filename, '\\'))) {
        *ptmp = '/';
    }    

   snprintf(full_path, sizeof(full_path), "%s/%s", fh_data->firehose_dir, unix_filename);
   if (access(full_path, R_OK)) {
	fh_cmd->program.num_partition_sectors = 0;
	dbg_time("fail to access %s, errno: %d (%s)\n", full_path, errno, strerror(errno));
	error_return();
   }

   fp = fopen(full_path, "rb");
   if (!fp) {
        fh_cmd->program.num_partition_sectors = 0;
        dbg_time("fail to fopen %s, errno: %d (%s)\n", full_path, errno, strerror(errno));
        error_return();
   }

   fseek(fp, 0, SEEK_END);
   filesize = ftell(fp);
   *filesize_out = filesize;
   fclose(fp);

   if (filesize <= 0) {
        dbg_time("fail to ftell %s, errno: %d (%s)\n", full_path, errno, strerror(errno));
        fh_cmd->program.num_partition_sectors = 0;
        fh_cmd->program.filesz = 0;
        error_return();
   }
   fh_cmd->program.filesz = filesize;

    fh_cmd->program.num_partition_sectors = filesize/fh_cmd->program.SECTOR_SIZE_IN_BYTES;
    if (filesize%fh_cmd->program.SECTOR_SIZE_IN_BYTES)
        fh_cmd->program.num_partition_sectors += 1;
    
    free(unix_filename);

    return 0;
}

static int fh_recv_cmd(struct fh_data *fh_data, struct fh_cmd *fh_cmd, unsigned timeout) {  
    int ret;
    char *xml_line;
    char *pend;

    memset(fh_cmd, 0, sizeof(struct fh_cmd));    
    	
    ret = fh_read(fh_data->edl_fd, fh_data->xml_buf, fh_data->xml_size, timeout);
    if (ret <= 0) {
	 	if (fh_ignore_read_timeout)
			return __LINE__;
        error_return();
    }
    fh_data->xml_buf[ret] = '\0';

   xml_line = fh_data->xml_buf;
    while (*xml_line) {
        xml_line = strstr(xml_line, "<?xml version=");
        if (xml_line == NULL) {
            if (fh_cmd->cmd.type == 0) {
                dbg_time("{{{%s}}}", fh_data->xml_buf);
                error_return();
            } else {
                break;
            }
        }
        xml_line += strlen("<?xml version=");

        xml_line = strstr(xml_line, "<data>\n");
        if (xml_line == NULL) {
            dbg_time("{{{%s}}}", fh_data->xml_buf);
            error_return();
        }
        xml_line += strlen("<data>\n");
        
        if (!strncmp(xml_line, "<response ", strlen("<response "))) {
            fh_parse_xml_line(xml_line, fh_cmd);
            pend = strstr(xml_line, "/>");
            pend += 2;
            dbg_time("%.*s\n", (int)(pend -xml_line),  xml_line);
            xml_line = pend + 1;
        }
        else if (!strncmp(xml_line, "<log ", strlen("<log "))) {
            if (fh_cmd->cmd.type && strcmp(fh_cmd->cmd.type, "log")) {
                dbg_time("{{{%s}}}", fh_data->xml_buf);
                break;
            }
            fh_parse_xml_line(xml_line, fh_cmd);
            pend = strstr(xml_line, "/>");
            pend += 2;
            {
                char *prn = xml_line;
                while (prn < pend) {
                    if (*prn == '\r' || *prn == '\n')
                        *prn = '.';
                    prn++;
                }
            }
            dbg_time("%.*s\n", (int)(pend -xml_line),  xml_line);
            xml_line = pend + 1;
        } else {
            dbg_time("unkonw %s", xml_line);
            error_return();
        }
    }

    if (fh_cmd->cmd.type)
        return 0;

    error_return();
}

static int fh_wait_response_cmd(struct fh_data *fh_data, struct fh_cmd *fh_cmd, unsigned timeout) { 
    while (1) {
        int ret = fh_recv_cmd(fh_data, fh_cmd, timeout);

        if (ret !=0)
            error_return();

        if (strstr(fh_cmd->cmd.type, "log"))
            continue;

        return 0;
    }

    error_return();
}

static int fh_send_cmd(struct fh_data *fh_data, const struct fh_cmd *fh_cmd) {  
    int tx_len = 0;
    char *pstart, *pend;
    char *xml_buf = fh_data->xml_buf;
    unsigned xml_size = fh_data->xml_size;
    xml_buf[0] = '\0';

    snprintf(xml_buf + strlen(xml_buf), xml_size, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n");
    snprintf(xml_buf + strlen(xml_buf), xml_size, "<data>\n");

    pstart = xml_buf + strlen(xml_buf);
    if (strstr(fh_cmd->cmd.type, "vendor")) {
        snprintf(xml_buf + strlen(xml_buf), xml_size, "%s", fh_cmd->vdef.buffer);
    }
    else if (strstr(fh_cmd->cmd.type, "erase")) {
        if (fh_cmd->erase.label[0] && fh_cmd->erase.last_sector)
        snprintf(xml_buf + strlen(xml_buf), xml_size, 
            "<erase PAGES_PER_BLOCK=\"%d\" SECTOR_SIZE_IN_BYTES=\"%d\" label=\"%s\" last_sector=\"%d\" num_partition_sectors=\"%d\" physical_partition_number=\"%d\" start_sector=\"%d\" />",  
            fh_cmd->erase.PAGES_PER_BLOCK, fh_cmd->erase.SECTOR_SIZE_IN_BYTES,
            fh_cmd->erase.label, fh_cmd->erase.last_sector,
            fh_cmd->erase.num_partition_sectors, fh_cmd->erase.physical_partition_number, fh_cmd->erase.start_sector);
        else if (fh_cmd->erase.PAGES_PER_BLOCK && fh_cmd->erase.SECTOR_SIZE_IN_BYTES)
        snprintf(xml_buf + strlen(xml_buf), xml_size, 
            "<erase PAGES_PER_BLOCK=\"%d\" SECTOR_SIZE_IN_BYTES=\"%d\" num_partition_sectors=\"%d\" physical_partition_number=\"%d\" start_sector=\"%d\"    />",  
            fh_cmd->erase.PAGES_PER_BLOCK, fh_cmd->erase.SECTOR_SIZE_IN_BYTES,
            fh_cmd->erase.num_partition_sectors, fh_cmd->erase.physical_partition_number, fh_cmd->erase.start_sector);
        else
        snprintf(xml_buf + strlen(xml_buf), xml_size, 
            "<erase num_partition_sectors=\"%u\" start_sector=\"%d\"    />",  
            fh_cmd->erase.num_partition_sectors, fh_cmd->erase.start_sector);
    }
    else if (strstr(fh_cmd->cmd.type, "program")) {
        if (fh_cmd->program.label[0] && fh_cmd->program.last_sector)
        snprintf(xml_buf + strlen(xml_buf), xml_size,
            "<program PAGES_PER_BLOCK=\"%d\" SECTOR_SIZE_IN_BYTES=\"%d\" filename=\"%s\" label=\"%s\" last_sector=\"%d\" num_partition_sectors=\"%d\"  physical_partition_number=\"%d\" start_sector=\"%d\" />",
            fh_cmd->program.PAGES_PER_BLOCK,  fh_cmd->program.SECTOR_SIZE_IN_BYTES,  fh_cmd->program.filename,
            fh_cmd->program.label, fh_cmd->program.last_sector,
            fh_cmd->program.num_partition_sectors, fh_cmd->program.physical_partition_number,  fh_cmd->program.start_sector);
        else        
        snprintf(xml_buf + strlen(xml_buf), xml_size,
            "<program PAGES_PER_BLOCK=\"%d\" SECTOR_SIZE_IN_BYTES=\"%d\" filename=\"%s\" num_partition_sectors=\"%d\"  physical_partition_number=\"%d\" start_sector=\"%d\" />",
            fh_cmd->program.PAGES_PER_BLOCK,  fh_cmd->program.SECTOR_SIZE_IN_BYTES,  fh_cmd->program.filename,
            fh_cmd->program.num_partition_sectors, fh_cmd->program.physical_partition_number,  fh_cmd->program.start_sector);
    }
    else if (strstr(fh_cmd->cmd.type, "configure")) {
        snprintf(xml_buf + strlen(xml_buf), xml_size,  
            "<configure MemoryName=\"%s\" Verbose=\"%d\" AlwaysValidate=\"%d\" MaxDigestTableSizeInBytes=\"%d\" MaxPayloadSizeToTargetInBytes=\"%d\"  ZlpAwareHost=\"%d\" SkipStorageInit=\"%d\" />",
            fh_cmd->cfg.MemoryName, fh_cmd->cfg.Verbose, fh_cmd->cfg.AlwaysValidate,
            fh_cmd->cfg.MaxDigestTableSizeInBytes, 
            fh_cmd->cfg.MaxPayloadSizeToTargetInBytes,
            fh_cmd->cfg.ZlpAwareHost, fh_cmd->cfg.SkipStorageInit);
    }
    else if (strstr(fh_cmd->cmd.type, "reset")) {
        snprintf(xml_buf + strlen(xml_buf), xml_size, "<power value=\"reset\" />");
    }
    else {
        dbg_time("%s unkonw fh_cmd->cmd.type=%s\n", __func__, fh_cmd->cmd.type);
        error_return();
    }
    
    pend = xml_buf + strlen(xml_buf);
    dbg_time("%.*s\n", (int)(pend - pstart),  pstart);
    
    snprintf(xml_buf + strlen(xml_buf), xml_size, "\n</data>");

    tx_len = fh_write(fh_data->edl_fd, xml_buf, strlen(xml_buf), 3000);

    if ((size_t)tx_len == strlen(xml_buf))
        return 0;

    error_return();
}

static int fh_send_reset_cmd(struct fh_data *fh_data) {
    struct fh_cmd fh_reset_cmd;

    fh_reset_cmd.cmd.type = "reset";

    return fh_send_cmd(fh_data, &fh_reset_cmd);
}

static int fh_send_cfg_cmd(struct fh_data *fh_data) {
    struct fh_cmd fh_cfg_cmd;
    struct fh_cmd fh_rx_cmd;

    memset(&fh_cfg_cmd, 0x00, sizeof(fh_cfg_cmd));
    fh_cfg_cmd.cfg.type = "configure";
    fh_cfg_cmd.cfg.MemoryName = "nand";
    fh_cfg_cmd.cfg.Verbose = 0;
    fh_cfg_cmd.cfg.AlwaysValidate = 0;
    fh_cfg_cmd.cfg.MaxDigestTableSizeInBytes = 2048;
    fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInBytes = 8*1024;
    fh_cfg_cmd.cfg.MaxPayloadSizeFromTargetInBytes = 2*1024;
    fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInByteSupported = 8*1024;
    fh_cfg_cmd.cfg.ZlpAwareHost = fh_data->ZlpAwareHost; // only sdx20 support zlp set to 0 by 20180822
    fh_cfg_cmd.cfg.SkipStorageInit = 0;

    fh_send_cmd(fh_data, &fh_cfg_cmd);
    if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 3000) != 0)
        error_return();

    if (!strcmp(fh_rx_cmd.response.value, "NAK") && fh_rx_cmd.response.MaxPayloadSizeToTargetInBytes) {
         fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInBytes = fh_rx_cmd.response.MaxPayloadSizeToTargetInBytes;
         fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInByteSupported = fh_rx_cmd.response.MaxPayloadSizeToTargetInBytes;

        fh_send_cmd(fh_data, &fh_cfg_cmd);
        if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 0) != 0)
            error_return();
    }

    if (strcmp(fh_rx_cmd.response.value, "ACK") != 0)
        error_return();

    fh_data->MaxPayloadSizeToTargetInBytes = fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInBytes;

    return 0;
}

static int fh_send_rawmode_image(struct fh_data *fh_data, const struct fh_cmd *fh_cmd, unsigned timeout) {
    char full_path[256];
    char *unix_filename = strdup(fh_cmd->program.filename);
    char *ptmp;
    FILE *fp;
    size_t filesize, filesend;
    void *pbuf = malloc(fh_data->MaxPayloadSizeToTargetInBytes);

    if (pbuf == NULL)
        error_return();
    
    while((ptmp = strchr(unix_filename, '\\'))) {
        *ptmp = '/';
    }

    snprintf(full_path, sizeof(full_path), "%s/%s", fh_data->firehose_dir, unix_filename);
    fp = fopen(full_path, "rb");
    if (!fp) {
        dbg_time("fail to fopen %s, errno: %d (%s)\n", full_path, errno, strerror(errno));
        error_return();
    }

    fseek(fp, 0, SEEK_END);
    filesize = ftell(fp);
    filesend = 0;
    fseek(fp, 0, SEEK_SET);        

    dbg_time("send %s, filesize=%zd\n", unix_filename, filesize);
    int idx = -1;
    while (filesend < filesize) {
        size_t reads = fread(pbuf, 1, fh_data->MaxPayloadSizeToTargetInBytes, fp);
        update_transfer_bytes(reads);
        if (!((++idx) % 32)) {
            printf(".");
            fflush(stdout);
        }

        if (reads > 0) {
            if (reads % fh_cmd->program.SECTOR_SIZE_IN_BYTES) {
                memset((uint8_t *)pbuf + reads, 0, fh_cmd->program.SECTOR_SIZE_IN_BYTES - (reads % fh_cmd->program.SECTOR_SIZE_IN_BYTES));
                reads +=  fh_cmd->program.SECTOR_SIZE_IN_BYTES - (reads % fh_cmd->program.SECTOR_SIZE_IN_BYTES);
            }
            size_t writes = fh_write(fh_data->edl_fd, pbuf, reads, timeout);                  
            if (reads != writes) {
                dbg_time("%s send fail reads=%zd, writes=%zd\n", __func__, reads, writes);
                dbg_time("%s send fail filesend=%zd, filesize=%zd\n", __func__, filesend, filesize);
                break;
            }
            filesend += reads;
            //dbg_time("filesend=%zd, filesize=%zd\n", filesend, filesize);
        } else {
            break;
        }
    }
    printf("\n");
    show_progress();
    dbg_time("send finished\n");

    fclose(fp);   
    free(unix_filename);
    free(pbuf);

    if (filesend >= filesize)
        return 0;

    error_return();
}

int firehose_main (const char *firehose_dir, int edl_fd) {
    unsigned x;
    char rawprogram_full_path[256];
    char *rawprogram_file;// = "rawprogram_nand_p4K_b256K_update.xml";
    struct fh_cmd fh_rx_cmd;
    struct fh_data *fh_data;
    long long filesizes = 0;
    long filesize = 0;
    unsigned max_num_partition_sectors = 0;

    fh_data = (struct fh_data *)malloc(sizeof(struct fh_data));
    if (!fh_data)
        error_return();
    
    memset(fh_data, 0x00, sizeof(struct fh_data));
    fh_data->firehose_dir = firehose_dir;
    fh_data->edl_fd = edl_fd;
    fh_data->xml_size = sizeof(fh_data->xml_buf);
    fh_data->ZlpAwareHost = 1;
	
    if(qfile_find_xmlfile(firehose_dir, "rawprogram", &rawprogram_file) != 0) {
        dbg_time("retrieve rawprogram namd file failed.\n");
        error_return();
    }
    
    snprintf(rawprogram_full_path, sizeof(rawprogram_full_path), "%s/%s", firehose_dir, rawprogram_file);
    free(rawprogram_file);
    
    if (access(rawprogram_full_path, R_OK)) {
        dbg_time("fail to access %s, errno: %d (%s)\n", rawprogram_full_path, errno, strerror(errno));
        error_return();
    }

    fh_parse_xml_file(fh_data, rawprogram_full_path);
    
    if (fh_data->fh_cmd_count == 0)
        error_return();

    for (x = 0; x < fh_data->fh_cmd_count; x++) {
        struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];
        
        if (strstr(fh_cmd->cmd.type, "program")) {            
            fh_fixup_program_cmd(fh_data, fh_cmd, &filesize);
            if (fh_cmd->program.num_partition_sectors == 0)
                error_return();
                
            //calc files size
            filesizes += filesize;
        }
        else if (strstr(fh_cmd->cmd.type, "erase")) {
            if ((fh_cmd->erase.num_partition_sectors + fh_cmd->erase.start_sector) > max_num_partition_sectors)
                max_num_partition_sectors = (fh_cmd->erase.num_partition_sectors + fh_cmd->erase.start_sector);   
        }
    }
	
	set_transfer_allbytes(filesizes);
	fh_ignore_read_timeout = 1;
	fh_recv_cmd(fh_data, &fh_rx_cmd, 3000);
	while (fh_recv_cmd(fh_data, &fh_rx_cmd, 1000) == 0);
	fh_ignore_read_timeout = 0;

    if (fh_send_cfg_cmd(fh_data))
        error_return();

    for (x = 0; x < fh_data->fh_cmd_count; x++) {
        const struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];

        if (strstr(fh_cmd->cmd.type, "vendor")) {
            fh_send_cmd(fh_data, fh_cmd);
            if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 6000) != 0) //SDX55 need 4 seconds
                error_return();
            if (strcmp(fh_rx_cmd.response.value, "ACK"))
                error_return();
        }
    }
    
    for (x = 0; x < fh_data->fh_cmd_count; x++) {
        struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];
        unsigned timeout = 6000;
        
        if (strstr(fh_cmd->cmd.type, "erase")) {
            fh_send_cmd(fh_data, fh_cmd);
            if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, timeout) != 0) //SDX55 need 4 seconds
                error_return();
            if (strcmp(fh_rx_cmd.response.value, "ACK"))
                error_return();
        }
    }
    
    for (x = 0; x < fh_data->fh_cmd_count; x++) {
        const struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];

        if (strstr(fh_cmd->cmd.type, "program")) {
            fh_send_cmd(fh_data, fh_cmd);
            if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 3000) != 0) {
                dbg_time("fh_wait_response_cmd fail\n");
                error_return();
             }
             if (strcmp(fh_rx_cmd.response.value, "ACK")) {
                dbg_time("response should be ACK\n");
                error_return();
             }
             if (fh_rx_cmd.response.rawmode != 1) {
                dbg_time("response should be rawmode true\n");
                error_return();
             }
             if (fh_send_rawmode_image(fh_data, fh_cmd, 15000)) {
                dbg_time("fh_send_rawmode_image fail\n");
                error_return();
             }
             if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 6000) != 0) {
                dbg_time("fh_wait_response_cmd fail\n");
                error_return();
             }
             if (strcmp(fh_rx_cmd.response.value, "ACK")) {
                dbg_time("response should be ACK\n");
                error_return();
             }
             if (fh_rx_cmd.response.rawmode != 0) {
                dbg_time("response should be rawmode false\n");
                error_return();
             }

            free(fh_cmd->program.filename);
        }
    }
    
    fh_send_reset_cmd(fh_data);
    if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 3000) != 0)
        error_return();
	fh_ignore_read_timeout = 1;
    while (fh_recv_cmd(fh_data, &fh_rx_cmd, 1000) == 0) {}; //required by sdx20
	fh_ignore_read_timeout = 0;

    free(fh_data);
	
    return 0;
}

int main(int argc, char *argv[]) {
	const char *mhi_dev = "0000:03:00.0";
	char mhi_chan[64];
	const char *firehose_dir = NULL;
	int fd;
	int ret;
	int opt;

///lib/firmware/firehose/prog_firehose_sdx55.mbn
	optind = 1;
    while ( -1 != (opt = getopt(argc, argv, "f:h"))) {
        switch (opt) {
			case 'f':
				firehose_dir = optarg;
			break;
			default:
			break;
		}
    	}

	if (access(firehose_dir, F_OK)) {
		dbg_time("fail to access %s, errno: %d (%s)\n", firehose_dir, errno, strerror(errno));
		error_return();
	}

	snprintf(mhi_chan, sizeof(mhi_chan), "/dev/mhi_%s_%s", mhi_dev, "EDL");
	if (!access(mhi_chan, F_OK))
		goto _find_edl;

	snprintf(mhi_chan, sizeof(mhi_chan), "/dev/mhi_%s_%s", mhi_dev, "DIAG");
	fd = open(mhi_chan, O_RDWR | O_NOCTTY);
	if (fd == -1) {
		dbg_time("fail to open %s, errno: %d (%s)\n", mhi_chan, errno, strerror(errno));
		error_return();
	}
	dbg_time("open('%s') = %d\n", mhi_chan, fd);

	dbg_time("send edl cmd\n");
	ret = write(fd, edl_cmd, sizeof(edl_cmd));
	if (ret != sizeof(edl_cmd)) {
		dbg_time("fail to write ret=%d, errno: %d (%s)\n", ret, errno, strerror(errno));
		close(fd);
		error_return();
	}

	while (ret > 0) {
		ret = read(fd, rx_buf, sizeof(rx_buf));
		dbg_time("read = %d\n", ret);
	}

	close(fd);

	snprintf(mhi_chan, sizeof(mhi_chan), "/dev/mhi_%s_%s", mhi_dev, "EDL");
	dbg_time("wait %s\n", mhi_chan);
	for (ret = 0; ret < 10; ret++) {
		if (!access(mhi_chan, F_OK))
			break;
		sleep(1);
	}

_find_edl:
	fd = open(mhi_chan, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd == -1) {
		dbg_time("fail to open %s, errno: %d (%s)\n", mhi_chan, errno, strerror(errno));
		error_return();
	}
	dbg_time("open('%s') = %d\n", mhi_chan, fd);

	ret = firehose_main(firehose_dir, fd);
	close(fd);

    dbg_time("Upgrade module %s.\n", ret == 0 ? "successfully" : "failed");

	return ret;	
}
