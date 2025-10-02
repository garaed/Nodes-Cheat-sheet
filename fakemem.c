#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdint.h>

/* желаемый объём памяти — 16 GiB */
#define FAKE_BYTES (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define FAKE_KB (FAKE_BYTES / 1024ULL)

static char tmp_meminfo_path[PATH_MAX] = {0};

/* реальные функции (с правильным приведением типов) */
static int  (*real_sysinfo)(struct sysinfo *) = NULL;
static long (*real_sysconf)(int) = NULL;
static int  (*real_open)(const char *, int, ...) = NULL;
static int  (*real_open64)(const char *, int, ...) = NULL;
static FILE *(*real_fopen)(const char *, const char *) = NULL;

/* Вспомогательная функция: записать временный /proc/meminfo */
static int write_tmp_meminfo(void) {
    char template[] = "/tmp/fakemem_meminfo_XXXXXX";
    int fd = mkstemp(template);
    if (fd == -1) return -1;

    /* сохраним путь */
    strncpy(tmp_meminfo_path, template, sizeof(tmp_meminfo_path)-1);

    FILE *real = fopen("/proc/meminfo", "r");
    if (!real) {
        /* если не получилось прочитать — создаём минимальный файл */
        FILE *out = fdopen(fd, "w");
        if (!out) { close(fd); unlink(tmp_meminfo_path); tmp_meminfo_path[0]=0; return -1; }
        fprintf(out, "MemTotal: %llu kB\n", (unsigned long long)FAKE_KB);
        fclose(out);
        chmod(tmp_meminfo_path, 0444);
        return 0;
    }

    FILE *out = fdopen(fd, "w");
    if (!out) { fclose(real); close(fd); unlink(tmp_meminfo_path); tmp_meminfo_path[0]=0; return -1; }

    char line[512];
    int replaced = 0;
    while (fgets(line, sizeof(line), real)) {
        if (!replaced && strncmp(line, "MemTotal:", 9) == 0) {
            fprintf(out, "MemTotal: %llu kB\n", (unsigned long long)FAKE_KB);
            replaced = 1;
        } else {
            fputs(line, out);
        }
    }
    if (!replaced) {
        fprintf(out, "MemTotal: %llu kB\n", (unsigned long long)FAKE_KB);
    }

    fclose(real);
    fflush(out);
    fclose(out);

    /* разрешаем чтение всем (необязательно) */
    chmod(tmp_meminfo_path, 0444);
    return 0;
}

/* Конструктор: инициализация */
static void __attribute__((constructor)) fakemem_init(void) {
    /* получить реальные символы с корректными приведениямии */
    real_sysinfo = (int (*)(struct sysinfo *)) dlsym(RTLD_NEXT, "sysinfo");
    real_sysconf = (long (*)(int)) dlsym(RTLD_NEXT, "sysconf");
    real_open    = (int (*)(const char *, int, ...)) dlsym(RTLD_NEXT, "open");
    real_open64  = (int (*)(const char *, int, ...)) dlsym(RTLD_NEXT, "open64");
    real_fopen   = (FILE *(*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen");

    /* Если open64 не найден (в некоторых системах), используем open как fallback */
    if (!real_open64) real_open64 = real_open;

    /* Забросим временный файл */
    if (write_tmp_meminfo() != 0) {
        /* не критично — библиотека всё равно попытается перехватить другие вызовы */
        tmp_meminfo_path[0] = 0;
    }
}

/* Деструктор: удалить временный файл */
static void __attribute__((destructor)) fakemem_fini(void) {
    if (tmp_meminfo_path[0]) {
        unlink(tmp_meminfo_path);
        tmp_meminfo_path[0] = 0;
    }
}

/* Перехват sysinfo() */
int sysinfo(struct sysinfo *info) {
    if (!real_sysinfo) real_sysinfo = (int (*)(struct sysinfo *)) dlsym(RTLD_NEXT, "sysinfo");
    int ret = 0;
    if (real_sysinfo)
        ret = real_sysinfo(info);
    else
        ret = -1;

    if (ret == 0 && info) {
        /* Попробуем выставить так, чтобы программа "видела" FAKE_BYTES.
           Заметим, что info->totalram имеет тип unsigned long: на 64-битных системах это OK.
           Также мы переопределяем sysconf(_SC_PHYS_PAGES) ниже, что покрывает многие случаи. */
        info->mem_unit = 1;
        /* безопасно привести (на 64-bit systems unsigned long достаточно большой) */
        info->totalram = (unsigned long) FAKE_BYTES;
        /* можно также не трогать freeram/available — оставляем реальные значения */
    }
    return ret;
}

/* Перехват sysconf() для _SC_PHYS_PAGES и _SC_AVPHYS_PAGES */
long sysconf(int name) {
    if (!real_sysconf) real_sysconf = (long (*)(int)) dlsym(RTLD_NEXT, "sysconf");

    if (name == _SC_PHYS_PAGES || name == _SC_AVPHYS_PAGES) {
        long page_size = 4096;
        if (real_sysconf) {
            long p = real_sysconf(_SC_PAGESIZE);
            if (p > 0) page_size = p;
        }
        unsigned long long pages = FAKE_BYTES / (unsigned long long)page_size;
        /* Ограничиваем возврат LONG_MAX если необходимо */
        if (pages > (unsigned long long)LONG_MAX) return LONG_MAX;
        return (long) pages;
    }

    if (real_sysconf) return real_sysconf(name);
    errno = ENOSYS;
    return -1;
}

/* Перехват open/open64 — перенаправляем /proc/meminfo на наш временный файл */
int open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    if (!real_open) real_open = (int (*)(const char *, int, ...)) dlsym(RTLD_NEXT, "open");

    if (pathname && tmp_meminfo_path[0] && strcmp(pathname, "/proc/meminfo") == 0) {
        return real_open(tmp_meminfo_path, flags, mode);
    }
    return real_open(pathname, flags, mode);
}

int open64(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    if (!real_open64) real_open64 = (int (*)(const char *, int, ...)) dlsym(RTLD_NEXT, "open64");
    if (!real_open64) real_open64 = (int (*)(const char *, int, ...)) dlsym(RTLD_NEXT, "open");

    if (pathname && tmp_meminfo_path[0] && strcmp(pathname, "/proc/meminfo") == 0) {
        return real_open64(tmp_meminfo_path, flags, mode);
    }
    return real_open64(pathname, flags, mode);
}

/* Перехват fopen — stdio обычно вызывает open, но на всякий случай */
FILE *fopen(const char *pathname, const char *mode) {
    if (!real_fopen) real_fopen = (FILE *(*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen");
    if (pathname && tmp_meminfo_path[0] && strcmp(pathname, "/proc/meminfo") == 0) {
        return real_fopen(tmp_meminfo_path, mode);
    }
    return real_fopen(pathname, mode);
}
