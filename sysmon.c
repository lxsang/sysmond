#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdint.h>
#include <getopt.h>
#include <stdlib.h>
#include <syslog.h>
#include <signal.h>
#include <sys/timerfd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/statvfs.h>
#include <math.h>

#include "ini.h"

#define DEFAULT_CONF_FILE (PREFIX "/etc/sysmond.conf")
#define MODULE_NAME "sysmon"
// #define DEFAULT_INPUT "/sys/class/hwmon/hwmon2/device/in3_input"
#define NET_INF_STAT_PT "/sys/class/net/%s/statistics/%s"

#define LOG_INIT(m) do { \
        setlogmask (LOG_UPTO (LOG_NOTICE)); \
        openlog ((m), LOG_CONS | LOG_PID | LOG_NDELAY, LOG_USER); \
    } while(0)


#define M_LOG(m, a,...) syslog ((LOG_NOTICE),m "_log@[%s: %d]: " a "\n", __FILE__, \
        __LINE__, ##__VA_ARGS__)

#define M_ERROR(m, a,...) syslog ((LOG_ERR),m "_error@[%s: %d]: " a "\n", __FILE__, \
        __LINE__, ##__VA_ARGS__)
        
#define JSON_FMT "{" \
            "\"stamp_sec\": %lu," \
            "\"stamp_usec\": %lu," \
            "\"battery\": %.3f," \
            "\"battery_percent\": %.3f," \
            "\"battery_max_voltage\": %d," \
            "\"battery_min_voltage\": %d," \
            "\"cpu_temp\": %d," \
            "\"gpu_temp\": %d," \
            "\"cpu_usages\":[%s]," \
            "\"mem_total\": %lu," \
            "\"mem_free\": %lu," \
            "\"mem_used\": %lu," \
            "\"mem_buff_cache\": %lu," \
            "\"mem_available\": %lu," \
            "\"mem_swap_total\": %lu," \
            "\"mem_swap_free\": %lu," \
            "\"disk_total\": %lu," \
            "\"disk_free\": %lu," \
            "\"net\":[%s]" \
        "}"

#define JSON_NET_FMT "{" \
            "\"name\":\"%s\"," \
            "\"rx\": %lu," \
            "\"tx\": %lu," \
            "\"rx_rate\": %.3f," \
            "\"tx_rate\": %.3f" \
        "},"

#define MAX_BUF 256
#define EQU(a,b) (strncmp(a,b,MAX_BUF) == 0)
#define MAX_NETWORK_INF 8
typedef struct
{
    char bat_in[MAX_BUF];
    uint16_t max_voltage;
    uint16_t min_voltage;
    uint16_t cutoff_voltage;
    float ratio;
    uint16_t read_voltage;
    float percent;
} sys_bat_t;

typedef struct
{
    char cpu_temp_file[MAX_BUF];
    char gpu_temp_file[MAX_BUF];
    uint16_t cpu;
    uint16_t gpu;
} sys_temp_t;

typedef struct {
    char name[32];
    unsigned long tx;
    unsigned long rx;
    float rx_rate;
    float tx_rate;
} sys_net_inf_t;

typedef struct {
    uint8_t n_intf;
    /*Monitor up to 8 interfaces*/
    sys_net_inf_t interfaces[MAX_NETWORK_INF];
} sys_net_t;

typedef struct
{
    unsigned long last_idle;
    unsigned long last_sum;
    float percent;
} sys_cpu_t;

typedef struct {
    char mount_path[MAX_BUF];
    unsigned long d_total;
    unsigned long d_free;
} sys_disk_t;

typedef struct 
{
    unsigned long m_total;
    unsigned long m_free;
    unsigned long m_available;
    unsigned long m_cache;
    unsigned long m_buffer;
    unsigned long m_swap_total;
    unsigned long m_swap_free;
} sys_mem_t;

typedef struct {
    char conf_file[MAX_BUF];
    char data_file_out[MAX_BUF];
    sys_bat_t bat_stat;
    sys_cpu_t* cpus;
    sys_mem_t mem;
    sys_temp_t temp;
    sys_net_t net;
    sys_disk_t disk;
    int n_cpus;
    struct itimerspec sample_period;
    int pwoff_cd;
    uint8_t power_off_percent;
} app_data_t;

static volatile int running = 1;
static char buf[MAX_BUF];

static void int_handler(int dummy)
{
    (void)dummy;
    running = 0;
}

static void help(const char *app)
{
    fprintf(stderr,
            "Usage: %s options.\n"
            "Options:\n"
            "\t -f <value>: config file\n"
            "\t -h <value>: this help message\n",
            app);
}

static void map(app_data_t* opt)
{
    float volt = opt->bat_stat.read_voltage*opt->bat_stat.ratio;
    if(volt < opt->bat_stat.min_voltage)
    {
        opt->bat_stat.percent = 0.0;
        return;
    }
    float result = 101 - (101 / pow(1 + pow(1.33 * (volt - opt->bat_stat.min_voltage) /
                                          (opt->bat_stat.max_voltage - opt->bat_stat.min_voltage), 4.5), 3));
    if(result > 100.0)
        result = 100.0;
    
    opt->bat_stat.percent = result;
}

static int guard_write(int fd, void* buffer, size_t size)
{
    int n = 0;
    int write_len;
    int st;
    while(n != (int)size)
    {
        write_len = (int)size - n;
        st = write(fd,buffer + n,write_len);
        if(st == -1)
        {
            M_ERROR(MODULE_NAME,"Unable to write to #%d: %s", fd, strerror(errno));
            return -1;
        }
        if(st == 0)
        {
            M_ERROR(MODULE_NAME,"Endpoint %d is closed", fd);
            return -1;
        }
        n += st;
    }
    return n;
}

static int read_line(int fd, char *buf, int size)
{
    int i = 0;
    char c = '\0';
    int n;
    while ((i < size - 1) && (c != '\n'))
    {
        n = read(fd, &c, 1);
        if (n > 0)
        {
            buf[i] = c;
            i++;
        }
        else
            c = '\n';
    }
    buf[i] = '\0';
    return i;
}

static int read_voltage(app_data_t* opts)
{
    int fd, ret;
    if(opts->bat_stat.bat_in[0] == '\0')
    {
        return 0;
    }
    fd = open(opts->bat_stat.bat_in, O_RDONLY);
    if(fd < 0)
    {
        M_ERROR(MODULE_NAME, "Unable to open input: %s", opts->bat_stat.bat_in);
        return -1;
    }
    (void)memset(buf, '\0', sizeof(buf));
    ret = read(fd, buf, sizeof(buf));
    if(ret > 0)
    {
        opts->bat_stat.read_voltage = atoi(buf);
        map(opts);
    }
    (void)close(fd);
    return 0;
}

static int read_cpu_info(app_data_t* opts)
{
    int fd, ret, j, i = 0;
    const char d[2] = " ";
    char* token;
    unsigned long sum = 0, idle = 0;
    fd = open("/proc/stat", O_RDONLY);
    if(fd < 0)
    {
        M_ERROR(MODULE_NAME, "Unable to open stat: %s", strerror(errno));
        return -1;
    }
    for (i = 0; i < opts->n_cpus; i++)
    {
        ret = read_line(fd, buf, MAX_BUF);
        if(ret > 0 && buf[0] == 'c' && buf[1] == 'p' && buf[2] == 'u')
        {
            token = strtok(buf,d);
            sum = 0;
            j = 0;
            while(token!=NULL)
            {
                token = strtok(NULL,d);
                if(token!=NULL){
                    sum += strtoul(token, NULL, 10);
                    if(j==3)
                        idle = strtoul(token, NULL, 10);
                    j++;
                }
            }
            opts->cpus[i].percent = 100 - (idle-opts->cpus[i].last_idle)*100.0/(sum-opts->cpus[i].last_sum);
            opts->cpus[i].last_idle = idle;
            opts->cpus[i].last_sum = sum;
        }
        else
        {
            M_ERROR(MODULE_NAME, "Unable to read CPU infos at: %d", i);
            break;
        }
    }
    (void) close(fd);
    if(i==0)
    {
        M_ERROR(MODULE_NAME, "No CPU info found");
        return -1;
    }
    return i;
}

static int read_mem_info(app_data_t* opts)
{
    int fd, ret;
    const char d[2] = " ";
    unsigned long data[7];
    char* token;
    fd = open("/proc/meminfo", O_RDONLY);
    if(fd < 0)
    {
        M_ERROR(MODULE_NAME, "Unable to open meminfo: %s", strerror(errno));
        return -1;
    }
    for (int i = 0; i < 7; i++) {
        ret = read_line(fd,buf,MAX_BUF);
        token = strtok(buf,d);
        token = strtok(NULL,d);
        if(token != NULL)
        {
            data[i] = (unsigned long)strtoul(token, NULL, 10);
        }
        else
        {
            data[i] = 0;
        }
        if(i == 4)
        {
            for (int j = 0; j < 9; j++) {
                ret = read_line(fd,buf,MAX_BUF);
                // skip 10 line
            }
        }
    }
    opts->mem.m_total       = data[0];
    opts->mem.m_free        = data[1];
    opts->mem.m_available   = data[2];
    opts->mem.m_buffer      = data[3];
    opts->mem.m_cache       = data[4];
    opts->mem.m_swap_total  = data[5];
    opts->mem.m_swap_free   = data[6];
    (void)ret;
    (void)close(fd);
    
    
    /*printf("total: %d used: %d, free: %d buffer/cache: %d, available: %d \n",
        opts->mem.m_total / 1024,
        (opts->mem.m_total - opts->mem.m_free - opts->mem.m_buffer-opts->mem.m_cache)/1024,
        opts->mem.m_free/1024,
        (opts->mem.m_buffer+opts->mem.m_cache)/1024,
        opts->mem.m_available/1024);*/
    return 0;
}

static int read_temp_file(const char* file, uint16_t* output)
{
    int fd, ret;
    if(file[0] != '\0')
    {
       fd = open(file, O_RDONLY);
        if(fd < 0)
        {
            M_ERROR(MODULE_NAME, "Unable to open temp file %s : %s", file, strerror(errno));
            return -1;
        }
        (void)memset(buf, '\0', sizeof(buf));
        ret = read(fd, buf, MAX_BUF);
        if(ret < 0)
        {
            M_ERROR(MODULE_NAME, "Unable to read temperature: %s", strerror(errno));
            (void) close(fd);
            return -1;
        }
        *output = (uint16_t)atoi(buf);
        (void) close(fd);
    }
    return 0;
}

static int read_cpu_temp(app_data_t* opts)
{
    if(read_temp_file(opts->temp.cpu_temp_file, &opts->temp.cpu) == -1)
    {
        return -1;
    }
    return read_temp_file(opts->temp.gpu_temp_file, &opts->temp.gpu);
}

static int read_net_statistic(app_data_t* opts)
{
    int fd, ret;
    float period;
    long unsigned int bytes;
    
    period = ((float)opts->sample_period.it_value.tv_nsec) / 1.0e9;
    for (int i = 0; i < opts->net.n_intf; i++)
    {
        // rx
        (void)snprintf(buf, MAX_BUF-1, NET_INF_STAT_PT, opts->net.interfaces[i].name, "rx_bytes");
        fd = open(buf, O_RDONLY);
        if(fd < 0)
        {
            M_ERROR(MODULE_NAME, "Unable to open %s: %s", buf, strerror(errno));
            return -1;
        }
        // read data to buff
        (void)memset(buf,'\0', MAX_BUF);
        ret = read(fd, buf, MAX_BUF);
        (void)close(fd);
        if(ret <= 0)
        {
            M_ERROR(MODULE_NAME, "Unable to read RX data of %s: %s", opts->net.interfaces[i].name, strerror(errno));
            return -1;
        }
        bytes = (unsigned long) strtoul(buf, NULL, 10);
        opts->net.interfaces[i].rx_rate = ((float)(bytes - opts->net.interfaces[i].rx) / period);
        opts->net.interfaces[i].rx = bytes;
        
        (void)snprintf(buf, MAX_BUF-1, NET_INF_STAT_PT, opts->net.interfaces[i].name, "tx_bytes");
        fd = open(buf, O_RDONLY);
        if(fd < 0)
        {
            M_ERROR(MODULE_NAME, "Unable to open %s: %s", buf, strerror(errno));
            return -1;
        }
        // read data to buff
        (void)memset(buf,'\0', MAX_BUF);
        ret = read(fd, buf, MAX_BUF);
        (void)close(fd);
        if(ret <= 0)
        {
            M_ERROR(MODULE_NAME, "Unable to read TX data of %s: %s", opts->net.interfaces[i].name, strerror(errno));
            return -1;
        }
        bytes = (unsigned long) strtoul(buf, NULL, 10);
        opts->net.interfaces[i].tx_rate = ((float)(bytes - opts->net.interfaces[i].tx) / period);
        opts->net.interfaces[i].tx = bytes ;
    }
    return 0;
}

static int read_disk_usage(app_data_t* opts)
{
    struct statvfs stat;
    int ret = statvfs(opts->disk.mount_path, &stat);
    if(ret < 0)
    {
        M_ERROR(MODULE_NAME, "Unable to query disk usage of %s: %s", opts->disk.mount_path, strerror(errno));
        return -1;
    }
    opts->disk.d_total = stat.f_blocks * stat.f_frsize;
    opts->disk.d_free = stat.f_bfree * stat.f_frsize;
    return 0;
}

static int log_to_file(app_data_t* opts)
{
    int ret,fd;
    char out_buf[1024];
    char net_buf[MAX_BUF];
    if(opts->data_file_out[0] == '\0')
    {
        return 0;
    }
    fd = open(opts->data_file_out, O_CREAT|O_WRONLY|O_APPEND | O_NONBLOCK, 0644);
    if(fd < 0)
    {
        M_ERROR(MODULE_NAME, "Unable to open output file: %s", strerror(errno));
        return -1;
    }
    (void)memset(buf,'\0',MAX_BUF);
    char* ptr = buf;
    // CPU
    size_t len = 0;
    for (int i = 0; i < opts->n_cpus; i++) {
        if(MAX_BUF - len -1 <= 0)
        {
            break;
        }
        snprintf(ptr, MAX_BUF - len -1, "%.3f,", opts->cpus[i].percent);
        len = strlen(buf);
        ptr = buf+len;
    }
    buf[len - 1] = '\0';
    
    // NET
    len = 0;
    ptr = net_buf;
    for (int i = 0; i < opts->net.n_intf; i++) {
        if(MAX_BUF - len -1 < strlen(JSON_NET_FMT))
        {
            break;
        }
        snprintf(ptr, MAX_BUF - len -1, JSON_NET_FMT,
            opts->net.interfaces[i].name,
            opts->net.interfaces[i].rx,
            opts->net.interfaces[i].tx,
            opts->net.interfaces[i].rx_rate,
            opts->net.interfaces[i].tx_rate
            );
        len = strlen(net_buf);
        ptr = net_buf+len;
    }
    net_buf[len - 1] = '\0';
    
    struct timeval now;
    gettimeofday(&now, NULL);
    snprintf(out_buf, sizeof(out_buf), JSON_FMT,
        now.tv_sec,
        now.tv_usec,
        opts->bat_stat.read_voltage* opts->bat_stat.ratio,
        opts->bat_stat.percent,
        opts->bat_stat.max_voltage,
        opts->bat_stat.min_voltage,
        opts->temp.cpu,
        opts->temp.gpu,
        buf,
        opts->mem.m_total,
        opts->mem.m_free,
        (opts->mem.m_total - opts->mem.m_free - opts->mem.m_buffer-opts->mem.m_cache),
        opts->mem.m_buffer+opts->mem.m_cache,
        opts->mem.m_available,
        opts->mem.m_swap_total,
        opts->mem.m_swap_free,
        opts->disk.d_total,
        opts->disk.d_free,
        net_buf
        );
    ret = guard_write(fd,out_buf,strlen(out_buf));
    if(ret <= 0)
    {
        M_ERROR(MODULE_NAME, "Unable to write data to output file");
        ret = -1;
    }
    if(ret != (int)strlen(out_buf))
    {
        M_ERROR(MODULE_NAME, "Unable to write all battery info to output file");
        ret = -1;
    }
    else
    {
        // M_LOG(MODULE_NAME, "written %d bytes to file: %s", strlen(out_buf), opts->data_file_out);
        ret = 0;
    }
    (void) close(fd);
    return ret;
}

static int ini_handle(void *user_data, const char *section, const char *name, const char *value)
{
    (void)section;
    unsigned long period = 0;
    const char d[2] = ",";
    char* token;
    
    app_data_t* opts = (app_data_t*) user_data;
    if(EQU(name, "battery_max_voltage"))
    {
        opts->bat_stat.max_voltage = atoi(value);
    }
    else if(EQU(name, "battery_min_voltage"))
    {
        opts->bat_stat.min_voltage = atoi(value);
    }
    else if(EQU(name, "battery_cutoff_votalge"))
    {
        opts->bat_stat.cutoff_voltage = atoi(value);
    }
    else if(EQU(name, "battery_divide_ratio"))
    {
        opts->bat_stat.ratio = atof(value);
    }
    else if(EQU(name, "battery_input"))
    {
        strncpy(opts->bat_stat.bat_in, value, MAX_BUF - 1);
    }
    else if(EQU(name, "sample_period"))
    {
        period = strtoul(value, NULL, 10)*1e6;
        opts->sample_period.it_interval.tv_nsec = period;
        opts->sample_period.it_value.tv_nsec = period;
    }
    else if(EQU(name, "cpu_core_number"))
    {
        opts->n_cpus = atoi(value) + 1;
    }
    else if(EQU(name, "power_off_count_down"))
    {
        opts->pwoff_cd = atoi(value);
    }
    else if(EQU(name, "power_off_percent"))
    {
        opts->power_off_percent = (uint8_t)atoi(value);
    }
    else if(EQU(name, "data_file_out"))
    {
        (void)strncpy(opts->data_file_out, value, MAX_BUF-1);
    }
    else if(EQU(name, "cpu_temperature_input"))
    {
        (void)strncpy(opts->temp.cpu_temp_file, value, MAX_BUF-1);
    }
    else if(EQU(name, "gpu_temperature_input"))
    {
        (void)strncpy(opts->temp.gpu_temp_file, value, MAX_BUF-1);
    }
    else if(EQU(name, "disk_mount_point"))
    {
        (void)strncpy(opts->disk.mount_path, value, MAX_BUF-1);
    }
    else if(EQU(name, "network_interfaces"))
    {
        // parsing the network interfaces
        token = strtok((char*)value,d);
        opts->net.n_intf = 0;
        while(token != NULL)
        {
            (void) strncpy(opts->net.interfaces[opts->net.n_intf].name, token, sizeof(opts->net.interfaces[opts->net.n_intf].name) - 1);
            opts->net.n_intf++;
            if(opts->net.n_intf >= MAX_NETWORK_INF)
                break;
            token = strtok(NULL,d);
        }
    }
    else
    {
        M_ERROR(MODULE_NAME, "Ignore unknown configuration %s = %s", name, value);
        return 0;
    }
    
    return 1;
}

static int load_config(app_data_t* opts)
{
    // global
    (void)memset(opts->data_file_out, '\0', MAX_BUF);
    (void)memset(opts->temp.cpu_temp_file, '\0', MAX_BUF);
    (void)memset(opts->temp.gpu_temp_file, '\0', MAX_BUF);
    opts->pwoff_cd = 5;
    opts->sample_period.it_interval.tv_sec = 0;
    opts->sample_period.it_interval.tv_nsec = 3e+8;
    opts->sample_period.it_value.tv_sec = 0;
    opts->sample_period.it_value.tv_nsec = 3e+8;
    opts->cpus = NULL;
    opts->n_cpus = 2;
    
    //battery
    (void)memset(opts->bat_stat.bat_in, '\0', MAX_BUF);
    opts->bat_stat.max_voltage = 4200;
    opts->bat_stat.min_voltage = 3300;
    opts->bat_stat.cutoff_voltage = 3000;
    opts->bat_stat.ratio = 1.0;
    opts->bat_stat.read_voltage = 0.0;
    opts->bat_stat.percent = 0.0;
    opts->power_off_percent = 1;
    
    
    (void)memset(&opts->mem, '\0', sizeof(opts->mem));
    (void)memset(&opts->temp, '\0', sizeof(opts->temp));
    (void)memset(&opts->net, '\0', sizeof(opts->net));
    (void)memset(&opts->disk, '\0', sizeof(opts->disk));
    opts->disk.mount_path[0] = '/';
    
    M_LOG(MODULE_NAME, "Use configuration: %s", opts->conf_file);
    if (ini_parse(opts->conf_file, ini_handle, opts) < 0)
    {
        M_ERROR(MODULE_NAME, "Can't load '%s'", opts->conf_file);
        return -1;
    }
    // check battery configuration
    if((opts->bat_stat.max_voltage < opts->bat_stat.min_voltage) ||
        (opts->bat_stat.max_voltage < opts->bat_stat.cutoff_voltage) ||
        (opts->bat_stat.min_voltage < opts->bat_stat.cutoff_voltage))
    {
        M_ERROR(MODULE_NAME, "Battery configuration is invalid: max: %d, min: %d, cut off: %d",
            opts->bat_stat.max_voltage,
            opts->bat_stat.min_voltage,
            opts->bat_stat.cutoff_voltage);
        return -1;
    }
    return 0;
}

int main(int argc, char *const *argv)
{
    int ret, tfd, count_down;
    float volt;
    uint64_t expirations_count;
    app_data_t opts;
    LOG_INIT(MODULE_NAME);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
    signal(SIGINT, int_handler);
    (void)strncpy(opts.conf_file, DEFAULT_CONF_FILE, MAX_BUF - 1);
    while ((ret = getopt(argc, argv, "hf:")) != -1)
    {
        switch (ret)
        {
        case 'f':
            (void)strncpy(opts.conf_file, optarg, MAX_BUF-1);
            break;
        default:
            help(argv[0]);
            return -1;
        }
    }
    
    if(optind > argc)
    {
        help(argv[0]);
        return -1;
    }
    
    if(load_config(&opts) != 0)
    {
        fprintf(stderr,"Unable to read config file\n");
        return -1;
    }
    
    M_LOG(MODULE_NAME, "Data Output: %s", opts.data_file_out);
    M_LOG(MODULE_NAME, "Battery input: %s", opts.bat_stat.bat_in);
    M_LOG(MODULE_NAME, "Battery Max voltage: %d", opts.bat_stat.max_voltage);
    M_LOG(MODULE_NAME, "Battery Min voltage: %d", opts.bat_stat.min_voltage);
    M_LOG(MODULE_NAME, "Battery Cut off voltage: %d", opts.bat_stat.cutoff_voltage);
    M_LOG(MODULE_NAME, "Battery Divide ratio: %.3f", opts.bat_stat.ratio);
    M_LOG(MODULE_NAME, "Sample period: %d", (int)(opts.sample_period.it_value.tv_nsec / 1e6));
    M_LOG(MODULE_NAME, "CPU cores: %d", opts.n_cpus);
    M_LOG(MODULE_NAME, "Power off count down: %d", opts.pwoff_cd);
    M_LOG(MODULE_NAME,"CPU temp. input: %s",opts.temp.cpu_temp_file);
    M_LOG(MODULE_NAME,"GPU temp. input: %s",opts.temp.gpu_temp_file);
    M_LOG(MODULE_NAME, "Poweroff percent: %d", opts.power_off_percent);
    
    // init timerfd
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (tfd == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to create timerfd: %s", strerror(errno));
        fprintf(stderr,"Unable to create timer fd: %s\n", strerror(errno));
        return -1;
    }
    if (timerfd_settime(tfd, 0 /* no flags */, &opts.sample_period, NULL) == -1)
    {
        M_ERROR(MODULE_NAME, "Unable to set framerate period: %s", strerror(errno));
        (void)close(tfd);
        return -1;
    }
    //init CPU monitors
    opts.cpus = (sys_cpu_t*) malloc(opts.n_cpus*sizeof(sys_cpu_t));
    for(int i=0; i < opts.n_cpus; i++)
    {
        opts.cpus[i].last_sum = 0;
        opts.cpus[i].last_idle = 0;
        opts.cpus[i].percent = 0.0;
    }
    // loop
    count_down = opts.pwoff_cd;
    while(running)
    {
        if(opts.bat_stat.bat_in[0] != '\0')
        {
            // open the file
            if(read_voltage(&opts) == -1)
            {
                M_ERROR(MODULE_NAME, "Unable to read system voltage");
            }
            volt = opts.bat_stat.read_voltage*opts.bat_stat.ratio;
            if(volt < opts.bat_stat.cutoff_voltage)
            {
                M_LOG(MODULE_NAME, "Invalid voltage read: %.3f", volt);
            }
            else
            {
                if(opts.bat_stat.percent <= (float)opts.power_off_percent)
                {
                    count_down--;
                    M_LOG(MODULE_NAME, "Out of battery. Will shutdown after %d count down", count_down);
                }
                else
                {
                    // reset the count_down
                    count_down = opts.pwoff_cd;
                }
                // check if we should shutdown
                if(count_down <= 0)
                {
                    M_LOG(MODULE_NAME, "Shutting down system");
                    ret = system("poweroff");
                    (void) ret;
                    // this should never happend
                    return 0;
                }
            }
        }
        // read cpu info
        if(read_cpu_info(&opts) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to read CPU infos");
        }
        // read memory usage
        if(read_mem_info(&opts) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to read memory usage");
        }
        // read CPU temperature
        if(read_cpu_temp(&opts) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to read CPU temperature");
        }
        if(read_net_statistic(&opts) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to query network statistic");
        }
        if(read_disk_usage(&opts) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to query disk usage");
        }
        // log to file
        if(log_to_file(&opts) == -1)
        {
            M_ERROR(MODULE_NAME, "Unable to write sysinfo to output");
        }
        // check timeout
        if(read(tfd, &expirations_count, sizeof(expirations_count)) != (int)sizeof(expirations_count))
        {
            M_ERROR(MODULE_NAME, "Unable to read timer: %s", strerror(errno));
        }
        else if (expirations_count > 1u)
        {
            M_ERROR(MODULE_NAME, "LOOP OVERFLOW COUNT: %lu", (long unsigned int)expirations_count);
        }
    }
    
    if(opts.cpus)
        free(opts.cpus);
    if(tfd > 0)
    {
        (void)close(tfd);
    }
    return 0;
}