#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char *path;
    char hash[41];
} IndexEntry;

typedef struct {
    IndexEntry *items;
    size_t len;
    size_t cap;
} IndexList;

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} PathList;

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

static char *dup_string(const char *text) {
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, len + 1);
    return copy;
}

static void strip_newlines(char *text) {
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}

static bool is_hash40(const char *text) {
    size_t i;
    if (strlen(text) != 40) {
        return false;
    }
    for (i = 0; i < 40; i++) {
        if (!isxdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

static char *shell_quote_alloc(const char *input) {
    size_t i;
    size_t len = 2;
    char *output;
    char *cursor;

    for (i = 0; input[i] != '\0'; i++) {
        len += (input[i] == '\'') ? 4 : 1;
    }

    output = malloc(len + 1);
    if (output == NULL) {
        return NULL;
    }

    cursor = output;
    *cursor++ = '\'';
    for (i = 0; input[i] != '\0'; i++) {
        if (input[i] == '\'') {
            memcpy(cursor, "'\\''", 4);
            cursor += 4;
        } else {
            *cursor++ = input[i];
        }
    }
    *cursor++ = '\'';
    *cursor = '\0';
    return output;
}

static int run_command_capture(const char *command, char *output, size_t output_size) {
    FILE *pipe;
    char chunk[512];
    size_t used = 0;
    int status;

    if (output != NULL && output_size > 0) {
        output[0] = '\0';
    }

    pipe = popen(command, "r");
    if (pipe == NULL) {
        return -1;
    }

    while (fgets(chunk, sizeof(chunk), pipe) != NULL) {
        size_t chunk_len = strlen(chunk);
        if (output != NULL && output_size > 0 && used < output_size - 1) {
            size_t available = output_size - 1 - used;
            size_t copy_len = chunk_len < available ? chunk_len : available;
            memcpy(output + used, chunk, copy_len);
            used += copy_len;
            output[used] = '\0';
        }
    }

    status = pclose(pipe);
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static int run_command_passthrough(const char *command) {
    int status = system(command);
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static int build_git_path(const char *repo_root, const char *entry, char *out, size_t out_size) {
    char git_dir[PATH_MAX];
    if (path_join(repo_root, ".git", git_dir, sizeof(git_dir)) != 0) {
        return -1;
    }
    if (entry == NULL || entry[0] == '\0') {
        return snprintf(out, out_size, "%s", git_dir) < (int)out_size ? 0 : -1;
    }
    return path_join(git_dir, entry, out, out_size);
}

static int absolute_to_repo_rel(const char *repo_root, const char *absolute_path, char *out, size_t out_size) {
    size_t root_len = strlen(repo_root);
    if (strncmp(repo_root, absolute_path, root_len) != 0) {
        return -1;
    }
    if (absolute_path[root_len] == '\0') {
        return snprintf(out, out_size, ".") < (int)out_size ? 0 : -1;
    }
    if (absolute_path[root_len] != '/') {
        return -1;
    }
    return snprintf(out, out_size, "%s", absolute_path + root_len + 1) < (int)out_size ? 0 : -1;
}

static int find_repo_root(char *out, size_t out_size) {
    char cursor[PATH_MAX];

    if (getcwd(cursor, sizeof(cursor)) == NULL) {
        return -1;
    }

    while (1) {
        char git_dir[PATH_MAX];
        struct stat st;

        if (build_git_path(cursor, "", git_dir, sizeof(git_dir)) == 0 &&
            stat(git_dir, &st) == 0 &&
            S_ISDIR(st.st_mode)) {
            if (realpath(cursor, out) != NULL) {
                return 0;
            }
            return snprintf(out, out_size, "%s", cursor) < (int)out_size ? 0 : -1;
        }

        if (strcmp(cursor, "/") == 0) {
            break;
        }

        {
            char *slash = strrchr(cursor, '/');
            if (slash == NULL) {
                break;
            }
            if (slash == cursor) {
                cursor[1] = '\0';
            } else {
                *slash = '\0';
            }
        }
    }

    return -1;
}

static void index_list_init(IndexList *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void index_list_free(IndexList *list) {
    size_t i;
    for (i = 0; i < list->len; i++) {
        free(list->items[i].path);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static int index_list_reserve(IndexList *list, size_t needed) {
    IndexEntry *new_items;
    size_t new_cap = list->cap == 0 ? 16 : list->cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    new_items = realloc(list->items, new_cap * sizeof(IndexEntry));
    if (new_items == NULL) {
        return -1;
    }
    list->items = new_items;
    list->cap = new_cap;
    return 0;
}

static ssize_t index_list_find(const IndexList *list, const char *path) {
    size_t i;
    for (i = 0; i < list->len; i++) {
        if (strcmp(list->items[i].path, path) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static int index_list_upsert(IndexList *list, const char *path, const char *hash) {
    ssize_t pos = index_list_find(list, path);
    if (pos >= 0) {
        memcpy(list->items[pos].hash, hash, 41);
        return 0;
    }

    if (list->len == list->cap && index_list_reserve(list, list->len + 1) != 0) {
        return -1;
    }

    list->items[list->len].path = dup_string(path);
    if (list->items[list->len].path == NULL) {
        return -1;
    }
    memcpy(list->items[list->len].hash, hash, 41);
    list->len++;
    return 0;
}

static int index_cmp_path(const void *left, const void *right) {
    const IndexEntry *l = (const IndexEntry *)left;
    const IndexEntry *r = (const IndexEntry *)right;
    return strcmp(l->path, r->path);
}

static int save_cg_index(const char *repo_root, IndexList *list) {
    char index_path[PATH_MAX];
    FILE *file;
    size_t i;

    if (build_git_path(repo_root, "cg-index", index_path, sizeof(index_path)) != 0) {
        return -1;
    }

    qsort(list->items, list->len, sizeof(IndexEntry), index_cmp_path);

    file = fopen(index_path, "w");
    if (file == NULL) {
        return -1;
    }

    for (i = 0; i < list->len; i++) {
        if (fprintf(file, "%s %s\n", list->items[i].hash, list->items[i].path) < 0) {
            fclose(file);
            return -1;
        }
    }

    return fclose(file) == 0 ? 0 : -1;
}

static int load_cg_index(const char *repo_root, IndexList *list) {
    char index_path[PATH_MAX];
    FILE *file;
    char *line = NULL;
    size_t cap = 0;
    ssize_t read_len;

    if (build_git_path(repo_root, "cg-index", index_path, sizeof(index_path)) != 0) {
        return -1;
    }

    file = fopen(index_path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    while ((read_len = getline(&line, &cap, file)) != -1) {
        char *space;
        char *hash;
        char *path;
        (void)read_len;
        strip_newlines(line);
        if (line[0] == '\0') {
            continue;
        }
        space = strchr(line, ' ');
        if (space == NULL) {
            continue;
        }
        *space = '\0';
        hash = line;
        path = space + 1;
        if (!is_hash40(hash) || path[0] == '\0') {
            continue;
        }
        if (index_list_upsert(list, path, hash) != 0) {
            free(line);
            fclose(file);
            return -1;
        }
    }

    free(line);
    return fclose(file) == 0 ? 0 : -1;
}

static void path_list_init(PathList *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void path_list_free(PathList *list) {
    size_t i;
    for (i = 0; i < list->len; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static bool path_list_contains(const PathList *list, const char *path) {
    size_t i;
    for (i = 0; i < list->len; i++) {
        if (strcmp(list->items[i], path) == 0) {
            return true;
        }
    }
    return false;
}

static int path_list_add(PathList *list, const char *path) {
    char **new_items;
    if (path_list_contains(list, path)) {
        return 0;
    }
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? 16 : list->cap * 2;
        new_items = realloc(list->items, new_cap * sizeof(char *));
        if (new_items == NULL) {
            return -1;
        }
        list->items = new_items;
        list->cap = new_cap;
    }
    list->items[list->len] = dup_string(path);
    if (list->items[list->len] == NULL) {
        return -1;
    }
    list->len++;
    return 0;
}

static int git_hash_object(const char *repo_root, const char *relpath, bool write_object, char out_hash[41]) {
    char *qroot = shell_quote_alloc(repo_root);
    char *qpath = shell_quote_alloc(relpath);
    char *command;
    char output[256];
    int status;
    size_t needed;
    const char *write_flag = write_object ? "-w" : "";

    if (qroot == NULL || qpath == NULL) {
        free(qroot);
        free(qpath);
        return -1;
    }

    needed = strlen("git -C  hash-object  --  2>/dev/null") + strlen(qroot) + strlen(write_flag) + strlen(qpath) + 1;
    command = malloc(needed);
    if (command == NULL) {
        free(qroot);
        free(qpath);
        return -1;
    }

    snprintf(command, needed, "git -C %s hash-object %s -- %s 2>/dev/null", qroot, write_flag, qpath);
    status = run_command_capture(command, output, sizeof(output));

    free(command);
    free(qroot);
    free(qpath);

    if (status != 0) {
        return -1;
    }

    strip_newlines(output);
    if (!is_hash40(output)) {
        return -1;
    }
    memcpy(out_hash, output, 41);
    return 0;
}

static int load_head_tree(const char *repo_root, IndexList *head_entries, bool *has_head) {
    char *qroot = shell_quote_alloc(repo_root);
    char *command;
    char output[256];
    size_t needed;
    int status;

    if (qroot == NULL) {
        return -1;
    }

    needed = strlen("git -C  rev-parse --verify HEAD 2>/dev/null") + strlen(qroot) + 1;
    command = malloc(needed);
    if (command == NULL) {
        free(qroot);
        return -1;
    }
    snprintf(command, needed, "git -C %s rev-parse --verify HEAD 2>/dev/null", qroot);
    status = run_command_capture(command, output, sizeof(output));
    free(command);

    if (status != 0) {
        *has_head = false;
        free(qroot);
        return 0;
    }

    *has_head = true;
    needed = strlen("git -C  ls-tree -r HEAD 2>/dev/null") + strlen(qroot) + 1;
    command = malloc(needed);
    if (command == NULL) {
        free(qroot);
        return -1;
    }
    snprintf(command, needed, "git -C %s ls-tree -r HEAD 2>/dev/null", qroot);

    {
        FILE *pipe = popen(command, "r");
        char *line = NULL;
        size_t cap = 0;
        ssize_t read_len;

        if (pipe == NULL) {
            free(command);
            free(qroot);
            return -1;
        }

        while ((read_len = getline(&line, &cap, pipe)) != -1) {
            char mode[16];
            char type[16];
            char hash[41];
            char *tab;
            char *path;
            (void)read_len;

            tab = strchr(line, '\t');
            if (tab == NULL) {
                continue;
            }
            *tab = '\0';
            path = tab + 1;
            strip_newlines(path);
            if (sscanf(line, "%15s %15s %40s", mode, type, hash) == 3) {
                if (strcmp(type, "blob") == 0 && is_hash40(hash) && path[0] != '\0') {
                    if (index_list_upsert(head_entries, path, hash) != 0) {
                        free(line);
                        pclose(pipe);
                        free(command);
                        free(qroot);
                        return -1;
                    }
                }
            }
        }

        free(line);
        status = pclose(pipe);
        if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            free(command);
            free(qroot);
            return -1;
        }
    }

    free(command);
    free(qroot);
    return 0;
}

static int sync_cg_index_from_head(const char *repo_root) {
    IndexList head_entries;
    bool has_head = false;
    int result;

    index_list_init(&head_entries);
    if (load_head_tree(repo_root, &head_entries, &has_head) != 0) {
        index_list_free(&head_entries);
        return -1;
    }

    result = save_cg_index(repo_root, &head_entries);
    index_list_free(&head_entries);
    return result;
}

static int get_current_branch(const char *repo_root, char *branch, size_t branch_size) {
    char *qroot = shell_quote_alloc(repo_root);
    char *command;
    char output[256];
    int status;
    size_t needed;

    if (qroot == NULL) {
        return -1;
    }

    needed = strlen("git -C  symbolic-ref --short HEAD 2>/dev/null") + strlen(qroot) + 1;
    command = malloc(needed);
    if (command == NULL) {
        free(qroot);
        return -1;
    }

    snprintf(command, needed, "git -C %s symbolic-ref --short HEAD 2>/dev/null", qroot);
    status = run_command_capture(command, output, sizeof(output));
    free(command);
    free(qroot);

    if (status != 0) {
        return snprintf(branch, branch_size, "detached") < (int)branch_size ? 0 : -1;
    }

    strip_newlines(output);
    return snprintf(branch, branch_size, "%s", output) < (int)branch_size ? 0 : -1;
}

static int collect_files_recursive(const char *repo_root, const char *absolute_path, PathList *files) {
    struct stat st;

    if (lstat(absolute_path, &st) != 0) {
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir;
        struct dirent *entry;
        char git_dir[PATH_MAX];

        if (build_git_path(repo_root, "", git_dir, sizeof(git_dir)) != 0) {
            return -1;
        }
        if (strcmp(absolute_path, git_dir) == 0) {
            return 0;
        }

        dir = opendir(absolute_path);
        if (dir == NULL) {
            return -1;
        }

        while ((entry = readdir(dir)) != NULL) {
            char next_path[PATH_MAX];
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (path_join(absolute_path, entry->d_name, next_path, sizeof(next_path)) != 0) {
                closedir(dir);
                return -1;
            }
            if (collect_files_recursive(repo_root, next_path, files) != 0) {
                closedir(dir);
                return -1;
            }
        }

        return closedir(dir) == 0 ? 0 : -1;
    }

    if (S_ISREG(st.st_mode)) {
        char relpath[PATH_MAX];
        if (absolute_to_repo_rel(repo_root, absolute_path, relpath, sizeof(relpath)) != 0) {
            return -1;
        }
        return path_list_add(files, relpath);
    }

    return 0;
}

static int collect_add_inputs(const char *repo_root, int argc, char **argv, PathList *files) {
    int i;
    char cwd[PATH_MAX];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return -1;
    }

    for (i = 0; i < argc; i++) {
        char joined[PATH_MAX];
        char resolved[PATH_MAX];
        char relpath[PATH_MAX];

        if (argv[i][0] == '/') {
            if (snprintf(joined, sizeof(joined), "%s", argv[i]) >= (int)sizeof(joined)) {
                return -1;
            }
        } else {
            if (path_join(cwd, argv[i], joined, sizeof(joined)) != 0) {
                return -1;
            }
        }

        if (realpath(joined, resolved) == NULL) {
            fprintf(stderr, "cg add: path not found: %s\n", argv[i]);
            return -1;
        }

        if (absolute_to_repo_rel(repo_root, resolved, relpath, sizeof(relpath)) != 0) {
            fprintf(stderr, "cg add: path outside repository: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(relpath, ".") == 0) {
            if (collect_files_recursive(repo_root, resolved, files) != 0) {
                return -1;
            }
        } else {
            if (collect_files_recursive(repo_root, resolved, files) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static void print_usage(void) {
    puts("CG - C Git");
    puts("Usage:");
    puts("  cg init [directory]");
    puts("  cg status");
    puts("  cg add <path> [path...]");
    puts("  cg commit -m <message>");
    puts("  cg log");
    puts("  cg branch [name]");
    puts("  cg branch -d <name>");
    puts("  cg checkout <branch|commit>");
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

    if (path_join(git_dir, "cg-index", path_buf, sizeof(path_buf)) != 0 ||
        write_text_file(path_buf, "") != 0) {
        fprintf(stderr, "cg init: cannot create cg-index\n");
        return 1;
    }

    if (realpath(git_dir, resolved) != NULL) {
        printf("Initialized empty CG repository in %s\n", resolved);
    } else {
        printf("Initialized empty CG repository in %s\n", git_dir);
    }

    return 0;
}

static int cmd_status(int argc, char **argv) {
    char repo_root[PATH_MAX];
    char branch[128];
    IndexList staged;
    IndexList head_entries;
    PathList working_files;
    PathList staged_new;
    PathList staged_modified;
    PathList staged_deleted;
    PathList unstaged_modified;
    PathList unstaged_deleted;
    PathList untracked;
    bool has_head = false;
    size_t i;

    if (argc != 0) {
        fprintf(stderr, "cg status: no arguments expected\n");
        return 1;
    }
    (void)argv;

    if (find_repo_root(repo_root, sizeof(repo_root)) != 0) {
        fprintf(stderr, "cg status: not inside a CG repository\n");
        return 1;
    }

    if (get_current_branch(repo_root, branch, sizeof(branch)) != 0) {
        fprintf(stderr, "cg status: cannot determine current branch\n");
        return 1;
    }

    index_list_init(&staged);
    index_list_init(&head_entries);
    path_list_init(&working_files);
    path_list_init(&staged_new);
    path_list_init(&staged_modified);
    path_list_init(&staged_deleted);
    path_list_init(&unstaged_modified);
    path_list_init(&unstaged_deleted);
    path_list_init(&untracked);

    if (load_cg_index(repo_root, &staged) != 0 || load_head_tree(repo_root, &head_entries, &has_head) != 0) {
        fprintf(stderr, "cg status: cannot read repository state\n");
        goto fail;
    }

    if (collect_files_recursive(repo_root, repo_root, &working_files) != 0) {
        fprintf(stderr, "cg status: cannot scan working tree\n");
        goto fail;
    }

    printf("On branch %s\n\n", branch);

    for (i = 0; i < staged.len; i++) {
        ssize_t head_pos = index_list_find(&head_entries, staged.items[i].path);
        if (head_pos < 0) {
            if (path_list_add(&staged_new, staged.items[i].path) != 0) {
                goto fail;
            }
        } else if (strcmp(staged.items[i].hash, head_entries.items[head_pos].hash) != 0) {
            if (path_list_add(&staged_modified, staged.items[i].path) != 0) {
                goto fail;
            }
        }
    }

    for (i = 0; i < head_entries.len; i++) {
        if (index_list_find(&staged, head_entries.items[i].path) < 0) {
            if (path_list_add(&staged_deleted, head_entries.items[i].path) != 0) {
                goto fail;
            }
        }
    }

    for (i = 0; i < staged.len; i++) {
        char absolute[PATH_MAX];
        char work_hash[41];
        if (path_join(repo_root, staged.items[i].path, absolute, sizeof(absolute)) != 0) {
            goto fail;
        }

        if (access(absolute, F_OK) != 0) {
            if (path_list_add(&unstaged_deleted, staged.items[i].path) != 0) {
                goto fail;
            }
            continue;
        }

        if (git_hash_object(repo_root, staged.items[i].path, false, work_hash) != 0) {
            goto fail;
        }
        if (strcmp(work_hash, staged.items[i].hash) != 0) {
            if (path_list_add(&unstaged_modified, staged.items[i].path) != 0) {
                goto fail;
            }
        }
    }

    for (i = 0; i < working_files.len; i++) {
        if (index_list_find(&staged, working_files.items[i]) < 0 &&
            index_list_find(&head_entries, working_files.items[i]) < 0) {
            if (path_list_add(&untracked, working_files.items[i]) != 0) {
                goto fail;
            }
        }
    }

    if (staged_new.len + staged_modified.len + staged_deleted.len > 0) {
        size_t j;
        puts("Changes to be committed:");
        for (j = 0; j < staged_new.len; j++) {
            printf("  new file:   %s\n", staged_new.items[j]);
        }
        for (j = 0; j < staged_modified.len; j++) {
            printf("  modified:   %s\n", staged_modified.items[j]);
        }
        for (j = 0; j < staged_deleted.len; j++) {
            printf("  deleted:    %s\n", staged_deleted.items[j]);
        }
        puts("");
    }

    if (unstaged_modified.len + unstaged_deleted.len > 0) {
        size_t j;
        puts("Changes not staged for commit:");
        for (j = 0; j < unstaged_modified.len; j++) {
            printf("  modified:   %s\n", unstaged_modified.items[j]);
        }
        for (j = 0; j < unstaged_deleted.len; j++) {
            printf("  deleted:    %s\n", unstaged_deleted.items[j]);
        }
        puts("");
    }

    if (untracked.len > 0) {
        size_t j;
        puts("Untracked files:");
        for (j = 0; j < untracked.len; j++) {
            printf("  %s\n", untracked.items[j]);
        }
        puts("");
    }

    if (staged_new.len + staged_modified.len + staged_deleted.len +
            unstaged_modified.len + unstaged_deleted.len + untracked.len ==
        0) {
        puts("nothing to commit, working tree clean");
    }

    index_list_free(&staged);
    index_list_free(&head_entries);
    path_list_free(&working_files);
    path_list_free(&staged_new);
    path_list_free(&staged_modified);
    path_list_free(&staged_deleted);
    path_list_free(&unstaged_modified);
    path_list_free(&unstaged_deleted);
    path_list_free(&untracked);
    return 0;

fail:
    index_list_free(&staged);
    index_list_free(&head_entries);
    path_list_free(&working_files);
    path_list_free(&staged_new);
    path_list_free(&staged_modified);
    path_list_free(&staged_deleted);
    path_list_free(&unstaged_modified);
    path_list_free(&unstaged_deleted);
    path_list_free(&untracked);
    return 1;
}

static int cmd_add(int argc, char **argv) {
    char repo_root[PATH_MAX];
    IndexList staged;
    PathList files;
    size_t i;

    if (argc < 1) {
        fprintf(stderr, "cg add: expected at least one path\n");
        return 1;
    }

    if (find_repo_root(repo_root, sizeof(repo_root)) != 0) {
        fprintf(stderr, "cg add: not inside a CG repository\n");
        return 1;
    }

    index_list_init(&staged);
    path_list_init(&files);

    if (load_cg_index(repo_root, &staged) != 0) {
        fprintf(stderr, "cg add: cannot read cg-index\n");
        goto fail;
    }

    if (collect_add_inputs(repo_root, argc, argv, &files) != 0) {
        goto fail;
    }

    if (files.len == 0) {
        fprintf(stderr, "cg add: no files matched\n");
        goto fail;
    }

    for (i = 0; i < files.len; i++) {
        char hash[41];
        if (git_hash_object(repo_root, files.items[i], true, hash) != 0) {
            fprintf(stderr, "cg add: failed to hash %s\n", files.items[i]);
            goto fail;
        }
        if (index_list_upsert(&staged, files.items[i], hash) != 0) {
            fprintf(stderr, "cg add: out of memory\n");
            goto fail;
        }
    }

    if (save_cg_index(repo_root, &staged) != 0) {
        fprintf(stderr, "cg add: cannot write cg-index\n");
        goto fail;
    }

    printf("staged %zu file(s)\n", files.len);

    index_list_free(&staged);
    path_list_free(&files);
    return 0;

fail:
    index_list_free(&staged);
    path_list_free(&files);
    return 1;
}

static int write_tree_from_index(const char *repo_root, const IndexList *staged, char out_tree[41]) {
    char template_path[PATH_MAX];
    int fd;
    size_t i;
    char *qroot = NULL;
    char *qtmp = NULL;
    char *command = NULL;
    char output[256];
    int status = -1;
    bool ok = false;

    if (build_git_path(repo_root, "cg-index-tmp-XXXXXX", template_path, sizeof(template_path)) != 0) {
        return -1;
    }

    fd = mkstemp(template_path);
    if (fd < 0) {
        return -1;
    }
    close(fd);

    qroot = shell_quote_alloc(repo_root);
    qtmp = shell_quote_alloc(template_path);
    if (qroot == NULL || qtmp == NULL) {
        goto cleanup;
    }

    {
        size_t needed = strlen("GIT_INDEX_FILE= git -C  read-tree --empty 2>/dev/null") + strlen(qtmp) + strlen(qroot) + 1;
        command = malloc(needed);
        if (command == NULL) {
            goto cleanup;
        }
        snprintf(command, needed, "GIT_INDEX_FILE=%s git -C %s read-tree --empty 2>/dev/null", qtmp, qroot);
        if (run_command_capture(command, output, sizeof(output)) != 0) {
            goto cleanup;
        }
        free(command);
        command = NULL;
    }

    for (i = 0; i < staged->len; i++) {
        char *qpath = shell_quote_alloc(staged->items[i].path);
        size_t needed;
        if (qpath == NULL) {
            goto cleanup;
        }
        needed = strlen("GIT_INDEX_FILE= git -C  update-index --add --cacheinfo 100644   2>/dev/null") +
                 strlen(qtmp) + strlen(qroot) + strlen(staged->items[i].hash) + strlen(qpath) + 1;
        command = malloc(needed);
        if (command == NULL) {
            free(qpath);
            goto cleanup;
        }
        snprintf(command, needed,
                 "GIT_INDEX_FILE=%s git -C %s update-index --add --cacheinfo 100644 %s %s 2>/dev/null",
                 qtmp,
                 qroot,
                 staged->items[i].hash,
                 qpath);
        free(qpath);
        if (run_command_capture(command, output, sizeof(output)) != 0) {
            goto cleanup;
        }
        free(command);
        command = NULL;
    }

    {
        size_t needed = strlen("GIT_INDEX_FILE= git -C  write-tree 2>/dev/null") + strlen(qtmp) + strlen(qroot) + 1;
        command = malloc(needed);
        if (command == NULL) {
            goto cleanup;
        }
        snprintf(command, needed, "GIT_INDEX_FILE=%s git -C %s write-tree 2>/dev/null", qtmp, qroot);
        if (run_command_capture(command, output, sizeof(output)) != 0) {
            goto cleanup;
        }
        strip_newlines(output);
        if (!is_hash40(output)) {
            goto cleanup;
        }
        memcpy(out_tree, output, 41);
    }

    ok = true;

cleanup:
    if (command != NULL) {
        free(command);
    }
    free(qroot);
    free(qtmp);
    status = unlink(template_path);
    if (status != 0 && errno != ENOENT) {
        return -1;
    }
    return ok ? 0 : -1;
}

static int cmd_commit(int argc, char **argv) {
    const char *message = NULL;
    int i;
    char repo_root[PATH_MAX];
    IndexList staged;
    char tree_hash[41];
    char parent_hash[41];
    char commit_hash[41];
    char branch[128];
    bool has_parent = false;
    char *qroot = NULL;
    char *qmsg = NULL;
    char *command = NULL;
    char output[512];

    if (find_repo_root(repo_root, sizeof(repo_root)) != 0) {
        fprintf(stderr, "cg commit: not inside a CG repository\n");
        return 1;
    }

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            message = argv[i + 1];
            i++;
        } else {
            fprintf(stderr, "cg commit: usage: cg commit -m <message>\n");
            return 1;
        }
    }

    if (message == NULL || message[0] == '\0') {
        fprintf(stderr, "cg commit: commit message is required\n");
        return 1;
    }

    index_list_init(&staged);
    if (load_cg_index(repo_root, &staged) != 0) {
        fprintf(stderr, "cg commit: cannot read cg-index\n");
        goto fail;
    }
    if (staged.len == 0) {
        fprintf(stderr, "cg commit: nothing staged\n");
        goto fail;
    }

    if (write_tree_from_index(repo_root, &staged, tree_hash) != 0) {
        fprintf(stderr, "cg commit: cannot write tree\n");
        goto fail;
    }

    qroot = shell_quote_alloc(repo_root);
    qmsg = shell_quote_alloc(message);
    if (qroot == NULL || qmsg == NULL) {
        fprintf(stderr, "cg commit: out of memory\n");
        goto fail;
    }

    {
        size_t needed = strlen("git -C  rev-parse --verify HEAD 2>/dev/null") + strlen(qroot) + 1;
        command = malloc(needed);
        if (command == NULL) {
            goto fail;
        }
        snprintf(command, needed, "git -C %s rev-parse --verify HEAD 2>/dev/null", qroot);
        if (run_command_capture(command, output, sizeof(output)) == 0) {
            strip_newlines(output);
            if (is_hash40(output)) {
                memcpy(parent_hash, output, 41);
                has_parent = true;
            }
        }
        free(command);
        command = NULL;
    }

    {
        size_t needed = strlen("GIT_AUTHOR_NAME='CG' GIT_AUTHOR_EMAIL='cg@local' "
                               "GIT_COMMITTER_NAME='CG' GIT_COMMITTER_EMAIL='cg@local' "
                               "git -C  commit-tree   -m  2>/dev/null") +
                        strlen(qroot) + strlen(tree_hash) + strlen(qmsg) + (has_parent ? strlen(parent_hash) + 4 : 0) + 1;
        command = malloc(needed);
        if (command == NULL) {
            goto fail;
        }
        if (has_parent) {
            snprintf(command,
                     needed,
                     "GIT_AUTHOR_NAME='CG' GIT_AUTHOR_EMAIL='cg@local' "
                     "GIT_COMMITTER_NAME='CG' GIT_COMMITTER_EMAIL='cg@local' "
                     "git -C %s commit-tree %s -p %s -m %s 2>/dev/null",
                     qroot,
                     tree_hash,
                     parent_hash,
                     qmsg);
        } else {
            snprintf(command,
                     needed,
                     "GIT_AUTHOR_NAME='CG' GIT_AUTHOR_EMAIL='cg@local' "
                     "GIT_COMMITTER_NAME='CG' GIT_COMMITTER_EMAIL='cg@local' "
                     "git -C %s commit-tree %s -m %s 2>/dev/null",
                     qroot,
                     tree_hash,
                     qmsg);
        }
        if (run_command_capture(command, output, sizeof(output)) != 0) {
            fprintf(stderr, "cg commit: cannot create commit object\n");
            goto fail;
        }
        strip_newlines(output);
        if (!is_hash40(output)) {
            fprintf(stderr, "cg commit: invalid commit hash\n");
            goto fail;
        }
        memcpy(commit_hash, output, 41);
        free(command);
        command = NULL;
    }

    {
        size_t needed = strlen("git -C  update-ref HEAD  2>/dev/null") + strlen(qroot) + strlen(commit_hash) + 1;
        command = malloc(needed);
        if (command == NULL) {
            goto fail;
        }
        snprintf(command, needed, "git -C %s update-ref HEAD %s 2>/dev/null", qroot, commit_hash);
        if (run_command_capture(command, output, sizeof(output)) != 0) {
            fprintf(stderr, "cg commit: cannot update HEAD\n");
            goto fail;
        }
        free(command);
        command = NULL;
    }

    {
        size_t needed = strlen("git -C  read-tree HEAD 2>/dev/null") + strlen(qroot) + 1;
        command = malloc(needed);
        if (command == NULL) {
            goto fail;
        }
        snprintf(command, needed, "git -C %s read-tree HEAD 2>/dev/null", qroot);
        (void)run_command_capture(command, output, sizeof(output));
        free(command);
        command = NULL;
    }

    if (sync_cg_index_from_head(repo_root) != 0) {
        fprintf(stderr, "cg commit: warning: failed to sync cg-index with HEAD\n");
    }

    if (get_current_branch(repo_root, branch, sizeof(branch)) != 0) {
        snprintf(branch, sizeof(branch), "detached");
    }

    printf("[%s %.7s] %s\n", branch, commit_hash, message);

    index_list_free(&staged);
    free(qroot);
    free(qmsg);
    return 0;

fail:
    if (command != NULL) {
        free(command);
    }
    free(qroot);
    free(qmsg);
    index_list_free(&staged);
    return 1;
}

static int cmd_log(int argc, char **argv) {
    char repo_root[PATH_MAX];
    char *qroot;
    char *command;
    char output[65536];
    size_t needed;
    int status;

    if (argc != 0) {
        fprintf(stderr, "cg log: no arguments expected\n");
        return 1;
    }
    (void)argv;

    if (find_repo_root(repo_root, sizeof(repo_root)) != 0) {
        fprintf(stderr, "cg log: not inside a CG repository\n");
        return 1;
    }

    qroot = shell_quote_alloc(repo_root);
    if (qroot == NULL) {
        return 1;
    }

    needed = strlen("git -C  --no-pager log --decorate --oneline --graph 2>/dev/null") + strlen(qroot) + 1;
    command = malloc(needed);
    if (command == NULL) {
        free(qroot);
        return 1;
    }

    snprintf(command, needed, "git -C %s --no-pager log --decorate --oneline --graph 2>/dev/null", qroot);
    status = run_command_capture(command, output, sizeof(output));
    free(command);
    free(qroot);

    if (status != 0 || output[0] == '\0') {
        puts("No commits yet.");
        return 0;
    }

    fputs(output, stdout);
    return 0;
}

static int cmd_branch(int argc, char **argv) {
    char repo_root[PATH_MAX];
    char *qroot;
    char *qname = NULL;
    char *command = NULL;
    size_t needed;
    int status;

    if (find_repo_root(repo_root, sizeof(repo_root)) != 0) {
        fprintf(stderr, "cg branch: not inside a CG repository\n");
        return 1;
    }

    qroot = shell_quote_alloc(repo_root);
    if (qroot == NULL) {
        return 1;
    }

    if (argc == 0) {
        needed = strlen("git -C  branch") + strlen(qroot) + 1;
        command = malloc(needed);
        if (command == NULL) {
            free(qroot);
            return 1;
        }
        snprintf(command, needed, "git -C %s branch", qroot);
        status = run_command_passthrough(command);
        free(command);
        free(qroot);
        return status == 0 ? 0 : 1;
    }

    if (argc == 1) {
        qname = shell_quote_alloc(argv[0]);
        if (qname == NULL) {
            free(qroot);
            return 1;
        }
        needed = strlen("git -C  branch ") + strlen(qroot) + strlen(qname) + 1;
        command = malloc(needed);
        if (command == NULL) {
            free(qname);
            free(qroot);
            return 1;
        }
        snprintf(command, needed, "git -C %s branch %s", qroot, qname);
        status = run_command_passthrough(command);
        free(command);
        free(qname);
        free(qroot);
        return status == 0 ? 0 : 1;
    }

    if (argc == 2 && strcmp(argv[0], "-d") == 0) {
        qname = shell_quote_alloc(argv[1]);
        if (qname == NULL) {
            free(qroot);
            return 1;
        }
        needed = strlen("git -C  branch -d ") + strlen(qroot) + strlen(qname) + 1;
        command = malloc(needed);
        if (command == NULL) {
            free(qname);
            free(qroot);
            return 1;
        }
        snprintf(command, needed, "git -C %s branch -d %s", qroot, qname);
        status = run_command_passthrough(command);
        free(command);
        free(qname);
        free(qroot);
        return status == 0 ? 0 : 1;
    }

    free(qroot);
    fprintf(stderr, "cg branch: usage: cg branch [name] | cg branch -d <name>\n");
    return 1;
}

static int cmd_checkout(int argc, char **argv) {
    char repo_root[PATH_MAX];
    char *qroot;
    char *qtarget;
    char *command;
    size_t needed;
    int status;

    if (argc != 1) {
        fprintf(stderr, "cg checkout: usage: cg checkout <branch|commit>\n");
        return 1;
    }

    if (find_repo_root(repo_root, sizeof(repo_root)) != 0) {
        fprintf(stderr, "cg checkout: not inside a CG repository\n");
        return 1;
    }

    qroot = shell_quote_alloc(repo_root);
    qtarget = shell_quote_alloc(argv[0]);
    if (qroot == NULL || qtarget == NULL) {
        free(qroot);
        free(qtarget);
        return 1;
    }

    needed = strlen("git -C  checkout ") + strlen(qroot) + strlen(qtarget) + 1;
    command = malloc(needed);
    if (command == NULL) {
        free(qroot);
        free(qtarget);
        return 1;
    }

    snprintf(command, needed, "git -C %s checkout %s", qroot, qtarget);
    status = run_command_passthrough(command);

    free(command);
    free(qroot);
    free(qtarget);

    if (status != 0) {
        return 1;
    }

    if (sync_cg_index_from_head(repo_root) != 0) {
        fprintf(stderr, "cg checkout: warning: failed to sync cg-index with HEAD\n");
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

    if (strcmp(argv[1], "status") == 0) {
        return cmd_status(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "add") == 0) {
        return cmd_add(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "commit") == 0) {
        return cmd_commit(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "log") == 0) {
        return cmd_log(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "branch") == 0) {
        return cmd_branch(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "checkout") == 0) {
        return cmd_checkout(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0) {
        puts("cg 0.2.0");
        return 0;
    }

    fprintf(stderr, "cg: command '%s' not implemented yet\n", argv[1]);
    return 1;
}
