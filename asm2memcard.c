/* MIT License
 *
 * Copyright (c) 2018 Noah Greenberg
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR_NONE 0
#define ERR_FILE_IN  1
#define ERR_FILE_OUT 2
#define ERR_POINTLESS 3
#define ERR_DIRECTIVE_BAD 4
#define ERR_DIRECTIVE_UNKNOWN 5
#define ERR_MALFORMED_ADDR 6
#define ERR_CODES_ALLOC 7
#define ERR_TITLE_LEN 8

#define GARBAGE_VALUE 0x8BADF00D
#define FREE_MEM_ADDR 0x80001800
#define FREE_END_ADDR 0x80002FFF
#define FREE_MEM_SIZE 0x17FF
#define NAME_TAG_ADDR 0x8045D850
#define NAME_END_ADDR 0x80469B94
#define NAME_TAG_SIZE 0xC344
#define CARD_MEM_ADDR 0x8045BF28
#define CARD_END_ADDR 0x8046B0EC
#define CARD_MEM_SIZE 0xF1C4

#define USER_ADDR 0x8045D930
#define INJECTION_SIZE 0x200 // More than enough to fit the launcher.
#define INJECTION_ADDR (CARD_END_ADDR - INJECTION_SIZE)

typedef enum directive {
    D_NONE,
    D_CHANGE,
    D_BRANCH,
    D_SET,
    D_TITLE
} directive;

typedef struct dir_data {
    uint32_t target;
    uint32_t home;
    uint32_t value;
} dir_data;

// NOTE: Dir len should be longer than any possible length.
#define DIRECTIVE_STR_LEN 8
#define SAVE_COMMENT_LEN 12

#define USER_CODES_INIT_LEN 512
uint32_t * user_codes_addr;
uint32_t * user_codes_val;
size_t user_codes_len = USER_CODES_INIT_LEN;
size_t user_codes_next = 0;

unsigned char save_comment[SAVE_COMMENT_LEN] = {0};
int save_comment_next = 0;

void error(int code, unsigned long line, const char * info) {
    if (line > 0) printf("ERR%d on line %d: ", code, line);
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

// Utility procedure to insert branch code after the user's.
void internal_asm_insert(uint32_t home, uint32_t * target, const uint32_t * c) {
    uint32_t t = *target;

    user_codes_push(home, find_branch(home, t));

    while (*c != 0) {
	user_codes_push(t, *c++);
	t += 4;
    }

    user_codes_push(t, find_branch(t, home + 4));
    t += 4;
    *target = t;
}

// bl instruction instead of b.
// Used for procedure calls.
#define find_lbranch(from, to) (find_branch(from, to) + 1)

// Handles hex values in user's script.
void a2m_feed_val(directive * did, dir_data * dat) {
    switch (*did) {
    case D_BRANCH:
	if (dat->home == 0) {
	    dat->home = 0x80000000 + dat->value;
	    user_codes_push(dat->home, find_branch(dat->home, dat->target));
	} else {
	    user_codes_push(dat->target, dat->value);
	    dat->target += 4;
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

// Initialize all data pertaining to the current directive.
void a2m_start_dir(directive did, dir_data * dat) {
    dat->home = 0;
    dat->value = 0;
    
    switch (did) {
    case D_TITLE:
	memset(save_comment, 0, sizeof(save_comment));
	save_comment_next = 0;
	break;
    }
}

// De-init all data pertaining to the current directive
// and write any data necessary for its end.
void a2m_end_dir(directive * did, dir_data * dat) {
    switch (*did) {
    case D_BRANCH:
	user_codes_push(dat->target, find_branch(dat->target, dat->home + 4));
	dat->target += 4;
	break;
    }
}

// Main script reading procedure.
// Not quite thread-safe yet!
void read_a2m(char * filename) {
    // Parsing data:
    FILE * src;
    unsigned long line = 1;
    unsigned char next, last;
    size_t readc = 0;
    bool in_rem = false;
    bool next_is_space;

    // Directive data:
    directive did = D_NONE;
    dir_data dat = {FREE_MEM_ADDR, 0, 0};
    unsigned char dir_str[DIRECTIVE_STR_LEN] = {0};
    int dir_str_i = 0;

    src = fopen(filename, "rb");
    if (!src) error(ERR_FILE_IN, 0, filename);

    do {
	// If at EOF, treat as null-terminator.
        readc = fread(&next, 1, 1, src);
        if (readc == 0) next = 0;
        else if (readc != 1) break;
	next_is_space = isspace(next);

	// Check if comment has started.
        if (next == ';') {
            in_rem = true;
            continue;
        }

	// Check if comment has ended.
        if (in_rem) {
            if (next == '\n') {
                in_rem = false;
                ++line;
            }
            continue;
        }

	// Check for the start of a new directive & end the current one.
	// End-of-file will be handled as if the directive is being changed.
	if (next == '@' || next == 0) {
	    if (!isspace(last)) a2m_feed_val(&did, &dat);
	    a2m_end_dir(&did, &dat);
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
		else if (strcmp(dir_str, "set") == 0)   did = D_SET;
		else if (strcmp(dir_str, "title") == 0) did = D_TITLE;
		else error(ERR_DIRECTIVE_UNKNOWN, line, dir_str);

		a2m_start_dir(did, &dat);

		// Clear string data for next directive change.
		memset(dir_str, 0, sizeof(dir_str));
		dir_str_i = 0;
	    } else {
                dir_str[dir_str_i++] = next;
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

    if (dat.target >= FREE_END_ADDR) {
        printf("WARNING: Codes are too large! Had to write beyond free memory area!\n");
    }

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

    fclose(src);
}

#define POKE(val)          fprintf(out, "04%06X %08X\r\n", target & 0xFFFFFF, val & 0xFFFFFFFF)
#define ATPOKE(addr, val)  fprintf(out, "04%06X %08X\r\n", addr & 0xFFFFFF, val & 0xFFFFFFFF)
#define INCPOKE(val)       fprintf(out, "04%06X %08X\r\n", (target += 4) & 0xFFFFFF, val & 0xFFFFFFFF)

// TODO: Allow usage of standard input / output.
int main(int argc, char ** argv) {
    FILE * out;
    uint32_t target = 0;

    if (argc < 3) {
        printf("Usage:   %s infile outfile\n"
               "Example: %s MyFunMods.a2m MyFunMods.txt\n\n"
               "The output should be used as an AR code in Dolphin.\n"
               "Run Melee with the code enabled and allow the game\n"
               "to save. The emulated memory card will have a save\n"
               "file that loads your codes.\n",
               argv[0], argv[0]);
        return -1;
    }

    out = fopen(argv[2], "wb");
    if (!out) error(ERR_FILE_OUT, 0, argv[2]);

    user_codes_addr = (uint32_t *)malloc(USER_CODES_INIT_LEN * sizeof(*user_codes_addr));
    user_codes_val  = (uint32_t *)malloc(USER_CODES_INIT_LEN * sizeof(*user_codes_val));
    if (!user_codes_addr || !user_codes_val) error(ERR_CODES_ALLOC, 0, 0);

    read_a2m(argv[1]);

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
    INCPOKE(0x9401FFFC); // stwu r0, -4(sp)
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
    INCPOKE(0x3D000000 + (((USER_ADDR + (user_codes_next + 1) * 4) >> 16) & 0xFFFF));
    INCPOKE(0x61080000 + ((USER_ADDR + (user_codes_next + 1) * 4) & 0xFFFF));
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

    INCPOKE(0x80010000); // lwz r0, 0(sp)
    INCPOKE(0x38210004); // addi sp, sp, 4
    INCPOKE(0x7C0803A6); // mtlr r0
    INCPOKE(0x4E800020); // blr

    return 0;
}
