#include <stdio.h>
#include <sys/sysinfo.h>
#include <unistd.h>
int main() {
    struct sysinfo s;
    if (sysinfo(&s) == 0) {
        printf("sysinfo totalram = %lu bytes, mem_unit=%u\n", (unsigned long)s.totalram, (unsigned int)s.mem_unit);
    }
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    printf("_SC_PHYS_PAGES=%ld, _SC_PAGESIZE=%ld, calc bytes=%llu\n", pages, psize, (unsigned long long)pages * (unsigned long long)psize);
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        int i=0;
        while (i++<6 && fgets(line,sizeof(line),f)) printf("%s",line);
        fclose(f);
    }
    return 0;
}
