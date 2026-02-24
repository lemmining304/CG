#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int path_join(const char *left, const char *right, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s/%s", left, right);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    return 0;
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0) {
        return 0;
    }

    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 0;
        }
    }

    return -1;
}

static int write_text_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }

    size_t content_len = strlen(content);
    size_t written = fwrite(content, 1, content_len, file);
    if (written != content_len) {
        fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        return -1;
    }

    return 0;
}

static void print_usage(void) {
    puts("CG - C Git (minimal)");
    puts("Usage:");
    puts("  cg init [directory]");
    puts("  cg --help");
    puts("  cg --version");
}

static int cmd_init(int argc, char **argv) {
    const char *target = ".";
    char git_dir[PATH_MAX];
    char path_buf[PATH_MAX];
    char resolved[PATH_MAX];
    const char *repo_dirs[] = {"objects", "refs", "refs/heads", "refs/tags"};
    size_t i;

    if (argc > 1) {
        fprintf(stderr, "cg init: too many arguments\n");
        return 1;
    }

    if (argc == 1) {
        target = argv[0];
    }

    if (ensure_dir(target) != 0) {
        fprintf(stderr, "cg init: cannot create/open directory '%s': %s\n", target, strerror(errno));
        return 1;
    }

    if (path_join(target, ".git", git_dir, sizeof(git_dir)) != 0) {
        fprintf(stderr, "cg init: path too long\n");
        return 1;
    }

    if (access(git_dir, F_OK) == 0) {
        fprintf(stderr, "cg init: repository already exists at '%s'\n", git_dir);
        return 1;
    }

    if (ensure_dir(git_dir) != 0) {
        fprintf(stderr, "cg init: cannot create '%s': %s\n", git_dir, strerror(errno));
        return 1;
    }

    for (i = 0; i < sizeof(repo_dirs) / sizeof(repo_dirs[0]); i++) {
        if (path_join(git_dir, repo_dirs[i], path_buf, sizeof(path_buf)) != 0) {
            fprintf(stderr, "cg init: path too long\n");
            return 1;
        }
        if (ensure_dir(path_buf) != 0) {
            fprintf(stderr, "cg init: cannot create '%s': %s\n", path_buf, strerror(errno));
            return 1;
        }
    }

    if (path_join(git_dir, "HEAD", path_buf, sizeof(path_buf)) != 0 ||
        write_text_file(path_buf, "ref: refs/heads/main\n") != 0) {
        fprintf(stderr, "cg init: cannot write HEAD\n");
        return 1;
    }

    if (path_join(git_dir, "config", path_buf, sizeof(path_buf)) != 0 ||
        write_text_file(path_buf,
                        "[core]\n"
                        "\trepositoryformatversion = 0\n"
                        "\tfilemode = true\n"
                        "\tbare = false\n"
                        "\tlogallrefupdates = true\n") != 0) {
        fprintf(stderr, "cg init: cannot write config\n");
        return 1;
    }

    if (path_join(git_dir, "description", path_buf, sizeof(path_buf)) != 0 ||
        write_text_file(path_buf, "Unnamed repository; edit this file to name it.\n") != 0) {
        fprintf(stderr, "cg init: cannot write description\n");
        return 1;
    }

    if (realpath(git_dir, resolved) != NULL) {
        printf("Initialized empty CG repository in %s\n", resolved);
    } else {
        printf("Initialized empty CG repository in %s\n", git_dir);
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "init") == 0) {
        return cmd_init(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0) {
        puts("cg 0.1.0");
        return 0;
    }

    fprintf(stderr, "cg: command '%s' not implemented yet\n", argv[1]);
    return 1;
}
