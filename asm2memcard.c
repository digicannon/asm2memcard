/* MIT License
 *
 * Copyright (c) 2021 Noah Greenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Please don't use this software to cheat or go against the wishes of tournament organizers.
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
// @TODO
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define EXEC_AS      "powerpc-eabi-as"
#define EXEC_OBJCOPY "powerpc-eabi-objcopy"

#define KEYCHAR_REM_START '#'
#define KEYCHAR_REM_MULTI '*'

#define ERR_NONE 0
#define ERR_FILE_IN  1
#define ERR_FILE_OUT 2
#define ERR_POINTLESS 3
#define ERR_DIRECTIVE_BAD 4
#define ERR_DIRECTIVE_UNKNOWN 5
#define ERR_MALFORMED_ADDR 6
#define ERR_CODES_ALLOC 7
#define ERR_TITLE_LEN 8
#define ERR_LAST_FREE_BANK 9
#define ERR_AS_FAILURE 10

typedef enum {
    COMPILE_TARGET_GECKO = 0,
    COMPILE_TARGET_DOLPHIN = 0xD,
    COMPILE_TARGET_NINTENDONT = 0x11,
} CompileTarget;
static CompileTarget compile_target = COMPILE_TARGET_GECKO;

static bool use_nametag_loader = false;

typedef struct {
    uint32_t start;
    uint32_t end;
} FreeMemoryRange;

static FreeMemoryRange free_memory[] = {
    {0x801910E0, 0x8019AF4C},
    {0x803FA3E8, 0x803FC2EC},
    {0x803001DC, 0x80301E44},
};
static const int free_memory_bank_count = 3;
static int free_memory_bank = 0;

#define GARBAGE_VALUE 0x8BADF00D
#define NAME_TAG_ADDR 0x8045D850
#define NAME_END_ADDR 0x80469B94
#define NAME_TAG_SIZE 0xC344
#define CARD_MEM_ADDR 0x8045BF28
#define CARD_END_ADDR 0x8046B0EC
#define CARD_MEM_SIZE 0xF1C4

#define USER_ADDR 0x8045D930
#define INJECTION_SIZE 0x200 // More than enough to fit the launcher.
#define INJECTION_ADDR (CARD_END_ADDR - INJECTION_SIZE)

typedef enum {
    REM_FALSE = 0,
    REM_TRUE  = 1,
    REM_MULTI,
    REM_MULTI_END, // Was MULTI but just got first semicolon.
    REM_NEW, // Was FALSE but current character turned to TRUE.
    REM_MAX // DO NOT USE.
} CommentMode;

typedef enum {
    D_NONE,
    D_CHANGE,
    D_BRANCH,
    D_ASM,
    D_FILE,
    D_SET,
    D_TITLE,
    D_INCLUDE,
    D_MAX // DO NOT USE.
} Directive;

typedef struct {
    uint32_t target;
    uint32_t home;
    uint32_t value;

    size_t user_codes_start;

    int asm_pipe[2];
    pid_t asm_pid;
    char * asm_out;

    char * asm_in;
    size_t asm_in_idx;
    size_t asm_in_size;
} DirData;

// NOTE: Dir len should be longer than any possible length.
#define DIRECTIVE_STR_LEN 8
#define SAVE_COMMENT_LEN 12

#define USER_CODES_INIT_LEN 512
uint32_t * user_codes_addr;
uint32_t * user_codes_val;
size_t user_codes_len = USER_CODES_INIT_LEN;
size_t user_codes_next = 0;

unsigned char save_comment[SAVE_COMMENT_LEN];
int save_comment_next = 0;

const char * dolphin_ini_original_path;
static FILE * dolphin_ini_original;
const char * dolphin_ini_temp_path;

#ifdef DEBUG
void dbg_print_with_dir_color(unsigned long line, char c, Directive did, CommentMode rem) {
    if (c == 0) {
        printf("\033[0m\n");
        return;
    }

    if (did == D_NONE) {
        printf("\033[0m");
    } else {
        printf("\033[%dm", 30 + (int)did);
    }

    if (rem) {
        //printf("\033[7;%dm", 30 + (int)rem);
        printf("\033[%dm", 40 + (int)rem);
    }

    if (c == '\n') {
        if (line != 1) printf("\u21B5");
        printf("\033[0m\n\033[47;30m%lu\033[0m\t", line);
    } else {
        printf("%c\033[0m", c);
    }
}

void dbg_print_key(const char * name, Directive did, CommentMode rem) {
    for (const char * c = name; *c; ++c) {
        dbg_print_with_dir_color(-1, *c, did, rem);
    }
    putchar(' ');
}
#else
#define dbg_print_with_dir_color(line, c, did, rem)
#define dbg_print_key(name, did, rem)
#endif

//#define DEFAULT_INI_DIR "/.local/share/dolphin-emu/GameSettings/"
#define DEFAULT_INI_DIR "/AppData/Roaming/Dolphin Emulator/GameSettings/"
#define DEFAULT_INI_NAME "GALE01.ini"
void dolphin_ini_find_path() {
    const char * home = getenv("HOME");
    char * temp;
    DIR * settings_dir;

    dolphin_ini_original_path = NULL;
    if (home == NULL) return;

    temp = calloc(strlen(home) + strlen(DEFAULT_INI_DIR DEFAULT_INI_NAME) + 1, 1);
    if (temp) {
        strcpy(temp, home);
        strcat(temp, DEFAULT_INI_DIR);
        settings_dir = opendir(temp);
#ifdef DEBUG
        printf("Opening %s ", temp);
        if (settings_dir) puts("OK");
        else              puts("failed");
#endif
        if (settings_dir) {
            closedir(settings_dir);
            strcat(temp, DEFAULT_INI_NAME);
            dolphin_ini_original_path = temp;
        }
    }

#ifdef DEBUG
    printf("Default INI path set to %s\n", dolphin_ini_original_path);
#endif
}

FILE * dolphin_ini_seek(const char * in_path, bool clean) {
    const size_t in_path_len = strlen(in_path);
    const size_t out_path_len = in_path_len + 4; // .tmp
    char * out_path;
    FILE * in;
    FILE * out;
    char buff[80];

    if (!clean) {
	in = fopen(in_path, "r");
        if (!in) {
            // @TODO error code
            fprintf(stderr, "Error %d: %s\n", errno, strerror(errno));
            fprintf(stderr, "Could not open Dolphin ini file at %s\n", in_path);
            return NULL;
        }
    }

    out_path = malloc(out_path_len + 1);
    if (!out_path) {
        fclose(in);
        return NULL;
    }
    memset(out_path, 0, out_path_len + 1);
    strcpy(out_path, in_path);
    strcat(out_path, ".tmp");
#ifdef DEBUG
    printf("Temp ini path: %s\n", out_path);
#endif

    out = fopen(out_path, "w");
    if (!out) {
        free(out_path);
        fclose(in);
        return NULL;
    }

    dolphin_ini_temp_path = out_path;

    if (clean) {
        fprintf(out, "[ActionReplay_Enabled]\n$%s\n", save_comment);
        if (!use_nametag_loader) {
            fprintf(out, "$Boot to Character Select [Dan Salvato]\n");
        }
        fprintf(out, "[ActionReplay]\n$%s\n", save_comment);
    } else {
        while (true) {
            if (fgets(buff, 80, in) == NULL) break;
            fprintf(out, "%s", buff); // Copy to temp.
            if (strcmp(buff, "[ActionReplay]\n") == 0) {
                fprintf(out, "$%s\n", save_comment);
                dolphin_ini_original = in; // dolphin_ini_close will write the rest.
                return out;
            }
        }

        // We never found the ActionReplay section, so make it.
        fprintf(out, "[ActionReplay]\n$%s\n", save_comment);
    }

    return out;
}

void dolphin_ini_close(FILE * out) {
    char buff[80];
    char name[SAVE_COMMENT_LEN + 2] = {'$'}; // One for $, another for newline.
    bool in_ar = true;
    bool in_old = false;

    if (!out || !dolphin_ini_original) return;

    memcpy(name + 1, save_comment, SAVE_COMMENT_LEN);
#ifdef DEBUG
    printf("Looking for old code named %s\n", name);
#endif
    strcat(name, "\n");

    while (true) {
        if (fgets(buff, 80, dolphin_ini_original) == NULL) break;

        if (in_ar) {
            if (buff[0] == '$') {
                in_old = strcmp(buff, name) == 0;
            } else if (buff[0] == '[') {
                in_ar = false;
                in_old = false;
            }

            // Copy to temp if not in our old output.
            if (!in_old) fprintf(out, "%s", buff);
        } else {
            fprintf(out, "%s", buff); // Copy to temp.
        }
    }

    fclose(dolphin_ini_original);
    fclose(out);
}

void error(int code, unsigned long line, const char * info) {
    if (line > 0) printf("ERR%d on line %lu: ", code, line);
    else          printf("ERR%d: ", code);

    switch (code) {
    default:
        printf("The fact that there is an error is an error. Please report this bug!\n");
        exit(-1);
    case ERR_FILE_IN:
    case ERR_FILE_OUT:
        printf("%s could not be opened!\n", info);
        break;
    case ERR_POINTLESS:
        printf("No codes were found in the input file.\n");
        break;
    case ERR_DIRECTIVE_BAD:
        printf("Data given with no directive.\n");
        break;
    case ERR_DIRECTIVE_UNKNOWN:
        printf("Unknown directive \"%s\".\n", info);
        break;
    case ERR_MALFORMED_ADDR:
        printf("Malformed address. Did you write a proper hex number? (NO 0x!)\n");
        break;
    case ERR_CODES_ALLOC:
        printf("Could not allocate enough memory to store user codes.\n");
        break;
    case ERR_TITLE_LEN:
        printf("Title length is too large! Must be smaller than %d.\n", SAVE_COMMENT_LEN);
        break;
    case ERR_LAST_FREE_BANK:
        printf("A bank switch was needed but there is no next bank to switch to.");
        break;
    case ERR_AS_FAILURE:
        printf("Failed to assemble input.  See above.");
        break;
    }

    exit(code);
}

void user_codes_push(uint32_t addr, uint32_t val) {
    if (user_codes_next >= user_codes_len) {
        user_codes_len += USER_CODES_INIT_LEN;
        user_codes_addr = (uint32_t *)realloc(user_codes_addr, user_codes_len * sizeof(*user_codes_addr));
        user_codes_val  = (uint32_t *)realloc(user_codes_val,  user_codes_len * sizeof(*user_codes_val));

        if (!user_codes_addr || !user_codes_val) error(ERR_CODES_ALLOC, 0, 0);
    }

    user_codes_addr[user_codes_next] = addr;
    user_codes_val[user_codes_next] = val;
    ++user_codes_next;
}

/* Returns opcode for branching from "from" to "to".
 * Difference between addresses must be
 * less than 0xffff and 4 byte aligned.
 * Returns nop instruction if from and to are equal.
 */
uint32_t find_branch(uint32_t from, uint32_t to) {
    if (from > to) {
        return 0x4C000000 - (from - to);
    } else if (to > from) {
        return 0x48000000 + (to - from);
    } else {
        return 0x60000000; // nop
    }
}

// bl instruction instead of b.
// Used for procedure calls.
#define find_lbranch(from, to) (find_branch(from, to) + 1)

// Handles hex values in user's script.
void a2m_feed_val(Directive * did, DirData * dat) {
    bool need_next_bank = false;

    switch (*did) {
    default: break;
    case D_BRANCH:
    case D_ASM:
    case D_FILE:
        if (dat->home == 0) {
            dat->home = 0x80000000 + dat->value;
            dat->user_codes_start = user_codes_next;
            user_codes_push(dat->home, find_branch(dat->home, dat->target));
        } else {
            user_codes_push(dat->target, dat->value);
            dat->target += 4;

            // Check if we need to switch free memory banks.
            // Add 4 for the branch back to home that will be pushed at the end.
            if (dat->target + 4 >= free_memory[free_memory_bank].end) {
                if (free_memory_bank + 1 >= free_memory_bank_count) {
                    error(ERR_LAST_FREE_BANK, 0, NULL);
                } else {
                    size_t wasted = free_memory[free_memory_bank].end - user_codes_addr[dat->user_codes_start + 1];
                    ++free_memory_bank;
                    printf("\nSwitching to free memory bank %d, wasting 0x%X bytes.", free_memory_bank + 1, wasted);
                    dat->target = free_memory[free_memory_bank].start;
                    size_t i = dat->user_codes_start;
                    // Recalculate the trampoline for this directive.
                    user_codes_val[i++] = find_branch(dat->home, dat->target);
                    // Relocate all the codes for this directive.
                    for (; i < user_codes_next; ++i) {
                        user_codes_addr[i] = dat->target;
                        dat->target += 4;
                    }
                }
            }
        }
        break;
    case D_SET:
        if (dat->home == 0) {
            dat->home = 0x80000000 + dat->value;
        } else {
            user_codes_push(dat->home, dat->value);
            *did = D_NONE;
        }
        break;
    }
}

void a2m_exec_as(bool use_in_file, DirData * dat) {
    pid_t pid;

    if (!use_in_file) pipe(dat->asm_pipe);
    pid = fork();

    if (pid == 0) {
        char * new_dir = NULL;
        char * filename = NULL;

        if (!use_in_file) {
            // The a2m file contains the asm.  We will pipe it in.
            dup2(dat->asm_pipe[0], STDIN_FILENO);
            close(dat->asm_pipe[0]);
            close(dat->asm_pipe[1]);
        } else {
            // We're assembling source on the file system.
            // chdir allows the source file to include things relative to it instead of a2m process.
            filename = strrchr(dat->asm_in, '/');
            if (filename) {
                // !!! MEMORY OWNERSHIP !!!
                // This code is not in control of this string but at the time of writing
                // the contents are unused until the next directive.  It is currently
                // safe to change its contents at this stage of the directive.
                // Look out for this in the future.
                new_dir = dat->asm_in; // filename points to inside of this string.
                *filename = 0; // Set the \ as the end of the directory path.
                ++filename; // Move past the \ which is now a null char.

                if (*filename == 0) {
                    // @TODO: Error out, user tried to assemble a directory.
                }

                chdir(new_dir);
            } else {
                // The file path is just a file name.
                filename = dat->asm_in;
            }
        }

        execlp(EXEC_AS, EXEC_AS,
                "-a32", "-mbig", "-mregnames",
                "-o", dat->asm_out,
                use_in_file ? filename : (const char *)NULL,
                (const char *)NULL);
        exit(1); // If we got here, exec failed.
    }

    if (!use_in_file) close(dat->asm_pipe[0]);
    if (pid > 0) {
        dat->asm_pid = pid;
    }
}

static char * append_to_cwd(const char * filename);
void a2m_exec_objcopy(Directive * did, DirData * dat, unsigned long a2m_line) {
    if (dat->asm_pid == -1 || dat->asm_pid == 0) return;
    if (dat->asm_out == NULL) return;

    int status;

    waitpid(dat->asm_pid, &status, 0);

    if (status) {
        // @TODO Actually error out.
        printf("as failed, code %d\n", status);
        error(ERR_AS_FAILURE, a2m_line, NULL);
    } else {
        pid_t obj_pid = fork();
        char * obj_out = append_to_cwd("/a.obj");

        if (obj_pid == 0) {
            execlp(EXEC_OBJCOPY, EXEC_OBJCOPY,
                    "-O", "binary", dat->asm_out, obj_out,
                    (const char *)NULL);
            exit(1); // If we got here, exec failed.
        }

        if (obj_pid > 0) {
            // Read output from objcopy.
            waitpid(obj_pid, &status, 0);
            if (status) {
                printf("objcopy failed, code %d\n", status);
            } else {
                int obj_out_handle = open(obj_out, O_RDONLY);
                if (obj_out_handle == -1) {
                    printf("failed to open objcopy output (%s), error %d\n", obj_out, errno);
                } else {
                    unsigned char buffer[1024];
                    ssize_t count;

                    while (true) {
                        count = read(obj_out_handle, buffer, 1024);

                        if (count == 0) break;
                        if (count == -1) {
                            // @TODO Output error.
                            break;
                        }

                        if (count % 4 != 0) {
                            // @TODO Print warning that last value will be ignored.
                        }

                        for (ssize_t i = 0; i < count; ++i) {
                            if (i % 4 == 0) dat->value = 0;
                            dat->value = (dat->value << 8) + buffer[i];
                            if (i % 4 == 3) {
                                a2m_feed_val(did, dat);
                            }
                        }
                    }

                    close(obj_out_handle);
                }
            }
        }

        dat->value = 0;
        free(dat->asm_out);
        dat->asm_out = NULL;
    }
}

// getcwd, but without an existing buffer.
// The buffer will resize until it can fit
// the current working directory, even if
// it is larger than PATH_MAX.
static char * sturdy_getcwd() {
    size_t size  = PATH_MAX;
    char * path  = NULL;
    char * valid = NULL;

    for (; !valid; size += PATH_MAX) {
        path  = realloc(path, size);
        valid = getcwd(path, size);

        if (!valid && errno != ERANGE) {
            if (path) free(path);
            return NULL;
        }
    }

    return path;
}

static char * append_to_cwd(const char * filename) {
    char * cwd;
    char * ret;

    if (filename == NULL) return NULL;

    cwd = sturdy_getcwd();
    if (cwd == NULL) return NULL;

    ret = calloc(strlen(cwd) + strlen(filename) + 1, 1);
    if (ret) {
        strcpy(ret, cwd);
        strcat(ret, filename);
    }

    free(cwd);
    return ret;
}

// Initialize all data pertaining to the current directive.
void a2m_start_dir(Directive did, DirData * dat) {
    dat->home = 0;
    dat->value = 0;

    if (dat->asm_in == NULL) {
        dat->asm_in_size = 1024;
        dat->asm_in = malloc(dat->asm_in_size);
    }
    memset(dat->asm_in, 0, dat->asm_in_size);
    dat->asm_in_idx = 0;

#define ASM_OUT_NAME "/a.out"
    if (did == D_ASM) {
        dat->asm_out = append_to_cwd(ASM_OUT_NAME);
        a2m_exec_as(false, dat);
    } else if (did == D_FILE) {
        dat->asm_out = append_to_cwd(ASM_OUT_NAME);
    } else if (did == D_TITLE) {
        memset(save_comment, 0, sizeof(save_comment));
        save_comment_next = 0;
    }
}

// De-init all data pertaining to the current directive
// and write any data necessary for its end.
void a2m_end_dir(Directive * did, DirData * dat, unsigned long line) {
    if (*did == D_ASM) {
        close(dat->asm_pipe[1]);
        a2m_exec_objcopy(did, dat, line);
    } else if (*did == D_FILE) {
        a2m_exec_as(true, dat);
        a2m_exec_objcopy(did, dat, line);
    }

    if (*did == D_BRANCH || *did == D_ASM || *did == D_FILE) {
        user_codes_push(dat->target, find_branch(dat->target, dat->home + 4));
        dat->target += 4;
    }
}

// Main script reading procedure.
// Not quite thread-safe yet!
void read_a2m(const char * filename) {
    // Parsing data:
    FILE * src;
    unsigned long line = 1;
    char next, last;
    size_t readc = 0;
    CommentMode in_rem = REM_FALSE;
    bool next_is_space;

    // Directive data:
    Directive did = D_NONE;
    DirData dat = {free_memory[free_memory_bank].start};
    char dir_str[DIRECTIVE_STR_LEN] = {0};
    int dir_str_i = 0;

    src = fopen(filename, "r");
    if (!src) error(ERR_FILE_IN, 0, filename);

#ifdef DEBUG
    // Init parser debug.
    printf("=== Color Key ===\n");
    printf("Directives: ");
    dbg_print_key("D_NONE", D_NONE, REM_FALSE);
    dbg_print_key("D_CHANGE", D_CHANGE, REM_FALSE);
    dbg_print_key("D_BRANCH", D_BRANCH, REM_FALSE);
    dbg_print_key("D_ASM", D_ASM, REM_FALSE);
    dbg_print_key("D_FILE", D_FILE, REM_FALSE);
    dbg_print_key("D_SET", D_SET, REM_FALSE);
    dbg_print_key("D_TITLE", D_TITLE, REM_FALSE);
    dbg_print_key("D_INCLUDE", D_INCLUDE, REM_FALSE);
    printf("\nComment modes: ");
    dbg_print_key("REM_FALSE", D_NONE, REM_FALSE);
    dbg_print_key("REM_TRUE", D_NONE, REM_TRUE);
    dbg_print_key("REM_MULTI", D_NONE, REM_MULTI);
    dbg_print_key("REM_MULTI_END", D_NONE, REM_MULTI_END);
    dbg_print_key("REM_NEW", D_NONE, REM_NEW);
    printf("\n=== Begin Input ===");
    dbg_print_with_dir_color(1, '\n', did, in_rem);
#endif

    do {
        // If at EOF, treat as null-terminator.
        readc = fread(&next, 1, 1, src);
        if (readc == 0) next = 0;
        else if (readc != 1) break;
        next_is_space = isspace(next);

        dbg_print_with_dir_color(line + 1, next, did, in_rem);

        // Check if comment has started.
        if (!in_rem && next == KEYCHAR_REM_START) {
            in_rem = REM_NEW;
            continue;
        }

        // Update comment state.
        if (in_rem) {
            switch (in_rem) {
            case REM_TRUE:
                if (next == '\n') {
                    in_rem = REM_FALSE;
                }
                break;
            case REM_MULTI:
                if (next == KEYCHAR_REM_MULTI) {
                    in_rem = REM_MULTI_END;
                }
                break;
            case REM_MULTI_END:
                if (next == KEYCHAR_REM_START) {
                    in_rem = REM_FALSE;
                } else {
                    in_rem = REM_MULTI;
                }
                break;
            case REM_NEW:
                if (next == '\n') {
                    in_rem = REM_FALSE;
                } else if (next == KEYCHAR_REM_MULTI) {
                    in_rem = REM_MULTI;
                } else {
                    in_rem = REM_TRUE;
                }
                break;
            default: break;
            }

            // Some directives require newlines to signal
            // the end of the directive, so pass \n and \r on.
            if (next != '\n' && next != '\r') {
                continue;
            }
        }

        // Check for the start of a new directive & end the current one.
        // End-of-file will be handled as if the directive is being changed.
        if (next == '@' || next == 0) {
            if (!isspace(last)) a2m_feed_val(&did, &dat);
            a2m_end_dir(&did, &dat, line);
            did = D_CHANGE;
            continue;
        }

        // Now handle character on a per-directive basis.

        switch (did) {
        case D_NONE:
            // The only thing right now should be a directive change.
            // If the script says something else, it's gone bad.
            if (!isspace(next)) error(ERR_DIRECTIVE_BAD, line, 0);
            break;
        case D_CHANGE:
            // At a space, the directive string has ended.
            // Or, if the directive has become longer than expected,
            // end it and it will match as unknown.
            if (isspace(next) || dir_str_i >= (DIRECTIVE_STR_LEN - 1)) {
                // Convert to lowercase for easier matching.
                for (char * c = dir_str; *c; ++c) *c = tolower(*c);

                // Match string with directive ID.

                if (strcmp(dir_str, "branch") == 0)     did = D_BRANCH;
                else if (strcmp(dir_str, "asm") == 0)   did = D_ASM;
                else if (strcmp(dir_str, "file") == 0)  did = D_FILE;
                else if (strcmp(dir_str, "set") == 0)   did = D_SET;
                else if (strcmp(dir_str, "title") == 0) did = D_TITLE;
                else if (strcmp(dir_str, "include") == 0) did = D_INCLUDE;
                else error(ERR_DIRECTIVE_UNKNOWN, line, dir_str);

                a2m_start_dir(did, &dat);

                // Clear string data for next directive change.
                memset(dir_str, 0, sizeof(dir_str));
                dir_str_i = 0;
            } else {
                dir_str[dir_str_i++] = next;
            }

            break;
        case D_ASM:
            if (dat.home) {
                char out[2] = {next, 0};
                write(dat.asm_pipe[1], out, 1);
            } else {
                goto read_hex;
            }
            break;
        case D_FILE:
            if (dat.home) {
                if (next == '\n' || next == '\r') {
                    // This will trigger the assembler.
                    a2m_end_dir(&did, &dat, line);
                    did = D_NONE;
                } else {
                    // Appent character to path.
                    if (dat.asm_in_idx >= dat.asm_in_size - 1) {
                        dat.asm_in_size += 1024;
                        dat.asm_in = realloc(dat.asm_in, dat.asm_in_size);
                    }
                    dat.asm_in[dat.asm_in_idx++] = next;
                    dat.asm_in[dat.asm_in_idx] = '\0';
                }
            } else {
                goto read_hex;
            }
            break;
        case D_TITLE:
            // If end-of-line, the title is complete. Otherwise add character to title.

            if (next == '\n' || next == '\r') {
                did = D_NONE;
            } else if (next != 0) {
                // Subtract 1 to save room for null-terminator.
                if (save_comment_next >= SAVE_COMMENT_LEN - 1) error(ERR_TITLE_LEN, line, 0);
                save_comment[save_comment_next++] = next;
            }

            break;
        default:
        read_hex:
            // At this point, we must be reading hex values.

            if (next_is_space) {
                // Feed directive completed value, as long as there is a value.
                // If we just had space, there is no value.
                if (!isspace(last)) {
                    a2m_feed_val(&did, &dat);
                    dat.value = 0;
                }
            } else {
                // Convert hex string to uint32.
                dat.value = dat.value << 4;
                if (next >= '0' && next <= '9') dat.value += next - '0';
                else if (next >= 'A' && next <= 'F') dat.value += 10 + next - 'A';
                else if (next >= 'a' && next <= 'f') dat.value += 10 + next - 'a';
                else error(ERR_MALFORMED_ADDR, line, 0);
            }
        }

        last = next;
        if (next == '\n') ++line;
    } while (readc == 1);

    printf("Ended output at 0x%X", dat.target);
    if (dat.target < free_memory[free_memory_bank].end) {
        uint32_t remaining = free_memory[free_memory_bank].end - dat.target;
        printf(", 0x%X bytes remaining.  That is %d instructions.\n", remaining, remaining / 4);
    } else {
        printf("\nWARNING: Codes are too large! Had to write beyond free memory area!\n");
    }

    if (save_comment_next > 0) {
        // Convert save title string to groups of uint32.
        uint32_t char32 = 0;
        for (int i = 0; i < SAVE_COMMENT_LEN; ++i) {
            char32 <<= 8;
            char32 |= save_comment[i];
            if ((i + 1) % 4 == 0) {
                user_codes_push(0x803BAC3C + i - 3, char32);
                user_codes_push(0x803BAC5C + i - 3, char32);
                char32 = 0;
            }
        }

        // The above wrote to filename data at 803BAC5C, 60, and 64.
        // Zero out the rest!
        user_codes_push(0x803BAC68, 0);
        user_codes_push(0x803BAC6C, 0);
        user_codes_push(0x803BAC70, 0);
    }

    fclose(src);
}

static void poke_impl(FILE * out, uint32_t target, uint32_t val) {
    if (compile_target == COMPILE_TARGET_NINTENDONT) {
        fprintf(out, "0x%06X, 0x%08X,\n", target & 0xFFFFFF, (val) & 0xFFFFFFFF);
    } else {
        fprintf(out, "04%06X %08X\n", target & 0xFFFFFF, (val) & 0xFFFFFFFF);
    }
}

#define POKE(val)         poke_impl(out, target, val)
#define ATPOKE(addr, val) poke_impl(out, addr, val)
#define INCPOKE(val)      { target += 4; POKE(val); }

int main(int argc, char ** argv) {
    FILE * out;
    uint32_t target = 0;
    bool clean_dolphin_ini = false;

    int first_nonflag_arg = 0;

    if (argc < 3) {
        printf("Usage:   %s infile outfile\n"
                "Example: %s in.a2m out.txt\n\n"
                "The output should be used as an AR code in Dolphin.\n"
                "Run Melee with the code enabled and allow the game\n"
                "to save. The emulated memory card will have a save\n"
                "file that loads your codes.\n\n"
                "See the README for details.\n",
               argv[0], argv[0]);
        return -1;
    }

    for (int i = 1; i < argc; ++i) {
        const char * arg = argv[i];
        if (arg[0] != '-') {
            first_nonflag_arg = i;
            break;
        } else if (strcmp(arg, "--nintendont") == 0) {
            compile_target = COMPILE_TARGET_NINTENDONT;
        } else if (strcmp(arg, "--dolphin") == 0) {
            compile_target = COMPILE_TARGET_DOLPHIN;
        } else if (strcmp(arg, "--clean") == 0) {
            clean_dolphin_ini = true;
        } else if (strcmp(arg, "--loader") == 0) {
            use_nametag_loader = true;
        } else if (strcmp(arg, "--no-loader") == 0) {
            use_nametag_loader = false;
        } else {
                printf("Unknown option %s\n", arg);
            return -1;
        }
    }

    const char * arg_path_in = argv[first_nonflag_arg];
    const char * arg_path_out = (first_nonflag_arg + 1 < argc) ? argv[first_nonflag_arg + 1] : NULL;

#ifdef DEBUG
    printf("compile_target: 0x%X\n", compile_target);
    printf("use_nametag_loader: %d\n", use_nametag_loader);
    printf("arg_path_in:  %s\n", arg_path_in);
    printf("arg_path_out: %s\n", arg_path_out);
#endif

    user_codes_addr = (uint32_t *)malloc(USER_CODES_INIT_LEN * sizeof(*user_codes_addr));
    user_codes_val  = (uint32_t *)malloc(USER_CODES_INIT_LEN * sizeof(*user_codes_val));
    if (!user_codes_addr || !user_codes_val) error(ERR_CODES_ALLOC, 0, 0);

    read_a2m(arg_path_in);

    if (compile_target == COMPILE_TARGET_DOLPHIN) {
        dolphin_ini_find_path();
        out = dolphin_ini_seek(dolphin_ini_original_path, clean_dolphin_ini);
        if (!out) error(ERR_FILE_OUT, 0, dolphin_ini_original_path);
    } else {
        out = fopen(arg_path_out, "w");
        if (!out) error(ERR_FILE_OUT, 0, arg_path_out);
    }

    if (use_nametag_loader) {
        if (compile_target == COMPILE_TARGET_NINTENDONT) {
            printf("WARNING: Compile target is set to Nintendont while building a nametag loader.\n");
        }

        // Fill nametag data with D4 bytes of garbage to start overflow.
        // Note: 0x35 is 0xD4 / 4
        target = NAME_TAG_ADDR;
        for (int i = 0; i <= 0x35; ++i) {
            POKE(GARBAGE_VALUE);
            target += 4;
        }
        ATPOKE(0x45D924, 0x804EE8F8); // Original stack address.
        ATPOKE(0x45D928, INJECTION_ADDR);
        ATPOKE(0x45D92C, 0);

        // Remove memcard wait @ CheckToSaveData.
        // See "Waiting for SaveData" in notes.
        user_codes_push(0x8001CDA0, 0xBB010010);

        if (user_codes_next > 0) {
            // Insert all poked addresses, then a 0, then the corresponding values.

            target = USER_ADDR;
            for (int i = 0; i < user_codes_next; ++i) {
                POKE(user_codes_addr[i]);
                target += 4;
            }
            POKE(0);
            target += 4;
            for (int i = 0; i < user_codes_next; ++i) {
                POKE(user_codes_val[i]);
                target += 4;
            }
        } else {
            error(ERR_POINTLESS, 0, 0);
        }

        /* See "Waiting for SaveData" in notes.
         * Insert a branch at the end of MemCard_CheckToSaveData to
         * check if the memcard is ready to be wrote to.
         * If it is, start loading.
         * This prevents crashing from changing the save file name
         * during autosave.  Usually happens on slow memcards.
         */

        target = INJECTION_ADDR;
        POKE(0x38600054);    // li r3, 0x54 ; Coin SFX ID
        INCPOKE(0x388000FE); // li r4, 0xFE ; Max Volume
        INCPOKE(0x38A00080); // li r5, 0x80 ; ?, said to usually be 80
        INCPOKE(0x38C00000); // li r6, 0    ; ?
        INCPOKE(0x38E00000); // li r7, 0    ; Echo
        INCPOKE(find_lbranch(target, 0x8038CFF4)); // bl PlaySFX
        INCPOKE(0x3C608001); // lis r3, 0x8001
        INCPOKE(0x6063CDA0); // ori r3, r3, 0xCDA0
        uint32_t opcode_branch_check = find_branch(0x8001CDA0, target + 36);
        INCPOKE(0x3C800000 + ((opcode_branch_check >> 16) & 0xFFFF));
        INCPOKE(0x60840000 + (opcode_branch_check & 0xFFFF));
        INCPOKE(0x90830000); // stw r4, 0(r3)
        INCPOKE(0x7C0018AC); // dcbf r0, r3
        INCPOKE(0x7C001FAC); // icbi r0, r3
        INCPOKE(0x7C0004AC); // sync
        INCPOKE(0x4C00012C); // isync
        INCPOKE(find_branch(target, 0x80239E9C)); // Back to Melee!

        // Check if memcard is ready.
        INCPOKE(0x81DB0000); // lwz r14, 0(r27)
        INCPOKE(0x2C0E0001); // cmpwi r14, 1
        INCPOKE(0x41820008); // beq done
        INCPOKE(0x48000019); // bl begin
        INCPOKE(0xBB010010); // lmw r24, 0x0010(sp)
        INCPOKE(0x3DC08001); // lis r14, 0x8001
        INCPOKE(0x61CECDA4); // ori r14, r14, 0xCDA4
        INCPOKE(0x7DC903A6); // mtctr r14
        INCPOKE(0x4E800420); // bctr ; Return to CheckToSaveData

        // Memcard is ready.  Begin loading!
        INCPOKE(0x7C0802A6); // mflr r0
        INCPOKE(0x90010004); // stw r0, 4(sp)
        INCPOKE(0x9421FFF8); // stwu sp, -8(sp)
        INCPOKE(find_lbranch(target, 0x800236DC)); // bl Music_Stop

        /* The patcher assembly was taken from wParam's POKE patcher.
         * This simply reads the user's addresses and values and puts
         * them where they belong.  It also invalidates the cache
         * for each overwritten address to prevent bad branch prediction.
         */

        // lis r7, upper word of USER_ADDR
        // ori r7, r7, lower word of USER_ADDR
        // lis r8, upper word of USER_ADDR + user_codes_next + 1
        // ori r8, r8, lower word of USER_ADDR + user_codes_next + 1
        INCPOKE(0x3CE00000 + ((USER_ADDR >> 16) & 0xFFFF));
        INCPOKE(0x60E70000 + (USER_ADDR & 0xFFFF));
        INCPOKE(0x3D000000 + (uint32_t)(((USER_ADDR + (user_codes_next + 1) * 4) >> 16) & 0xFFFF));
        INCPOKE(0x61080000 + (uint32_t)((USER_ADDR + (user_codes_next + 1) * 4) & 0xFFFF));
        // patcher:
        INCPOKE(0x80670000); // lwz r3, 0(r7)
        INCPOKE(0x80880000); // lwz r4, 0(r8)
        INCPOKE(0x2C030000); // cmpwi r3, 0
        INCPOKE(0x41820028); // beq loader
        INCPOKE(0x90830000); // stw r4, 0(r3)
        INCPOKE(0x54630034); // rlwinm r3, r3, 0, 0, 26
        INCPOKE(0x7C0018AC); // dcbf r0, r3
        INCPOKE(0x7C001FAC); // icbi r0, r3
        INCPOKE(0x7C0004AC); // sync
        INCPOKE(0x4C00012C); // isync
        INCPOKE(0x38E70004); // addi r7, r7, 4
        INCPOKE(0x39080004); // addi r8, r8, 4
        INCPOKE(0x4BFFFFD0); // b patcher

        /* Here the nametag area is cleared before asking the game
         * to load/create a new save file.  This is explained below.
         * However, I'd like to give a huge shout out to the
         * Smashboards Community Symbol Map.  I would not have been
         * able to decipher procedure calls without it!
         */

        // loader:
        // lis r3, upper word of NAME_TAG_ADDDR
        // ori r3, r3, lower word of NAME_TAG_ADDR
        // li r4, 0
        // lis r5, upper word of NAME_TAG_SIZE
        // ori r5, r5, lower word of NAME_TAG_SIZE
        // bl memset
        INCPOKE(0x3C600000 + ((NAME_TAG_ADDR >> 16) & 0xFFFF));
        INCPOKE(0x60630000 + (NAME_TAG_ADDR & 0xFFFF));
        INCPOKE(0x38800000);
        INCPOKE(0x3CA00000 + ((NAME_TAG_SIZE >> 16) & 0xFFFF));
        INCPOKE(0x60A50000 + (NAME_TAG_SIZE & 0xFFFF));
        INCPOKE(find_lbranch(target, 0x80003130));

        /* Earlier, in the patch loop, the strings for memory card
         * saves have been overwritten to the user's custom strings.
         * As long as they are not 100% equal to Melee's, the save
         * file with the custom data will be loaded.  If it does not
         * already exist, the game will ask the user if they would
         * like to create one.  This save file will NOT include the
         * nametag overflow & should allow nametags to work normally.
         */
        INCPOKE(find_lbranch(target, 0x8015F600)); // bl InitializeNametagArea
        INCPOKE(find_lbranch(target, 0x8001CBBC)); // bl DoLoadData
        INCPOKE(0x38600054); // li r3, 0x54 ; Coin SFX ID
        INCPOKE(0x388000FE); // li r4, 0xFE ; Max Volume
        INCPOKE(0x38A00080); // li r5, 0x80 ; ?, said to usually be 80
        INCPOKE(0x38C00000); // li r6, 0    ; ?
        INCPOKE(0x38E00000); // li r7, 0    ; Echo
        INCPOKE(find_lbranch(target, 0x8038CFF4)); // bl PlaySFX
        INCPOKE(0x38600000); // li r3, 0 ; Title Screen ID
        INCPOKE(find_lbranch(target, 0x801A428C)); // bl NewMajor
        INCPOKE(find_lbranch(target, 0x801A4B60)); // bl SetGo

        INCPOKE(0x8001000C); // lwz r0, 0xC(sp)
        INCPOKE(0x38210008); // addi sp, sp, 8
        INCPOKE(0x7C0803A6); // mtlr r0
        INCPOKE(0x4E800020); // blr
    } else {
        for (int i = 0; i < user_codes_next; ++i) {
            uint32_t addr = user_codes_addr[i];
            uint32_t val = user_codes_val[i];
            ATPOKE(addr, val);
        }
    }

    if (compile_target == COMPILE_TARGET_DOLPHIN) {
        dolphin_ini_close(out);
#ifdef DEBUG
        printf("%s -> %s\n", dolphin_ini_temp_path, dolphin_ini_original_path);
#endif
        // @TODO Depending on the libc, this may be required:
        // remove(dolphin_ini_original_path);
        if (rename(dolphin_ini_temp_path, dolphin_ini_original_path)) {
            // @TODO error code
            fprintf(stderr, "Failed to copy temp ini file into place.\n");
            fprintf(stderr, "Error %d: %s\n", errno, strerror(errno));
        }
    }

    return 0;
}
