/* memtools: search parsing, watch list, trace filters (no ROMs needed). */
#include <stdio.h>
#include <string.h>

#include "smsmemtools.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void)
{
    uint8_t pat[SMSSEARCH_MAX], mask;
    int n, flags;
    smsdebug_trace t = { .pc = 0x8033, .len = 3, .bytes = { 0xCD, 0x50, 0x80 } };
    smsdebug_trace u = { .pc = 0x9000, .len = 1, .bytes = { 0x00 } };
    char buf[128];

    CHECK(smssearch_parse(SMSSEARCH_HEX, "5A 3c,$FF 0x10", pat, &n, &mask, &flags), "hex parses");
    CHECK(n == 4 && pat[0] == 0x5A && pat[1] == 0x3C && pat[2] == 0xFF && pat[3] == 0x10, "hex bytes");
    CHECK(mask == 0xFF && flags == 0, "hex mask/flags");
    CHECK(smssearch_parse(SMSSEARCH_HEX, "5", pat, &n, &mask, &flags) && n == 1 && pat[0] == 5, "lone digit");
    CHECK(!smssearch_parse(SMSSEARCH_HEX, "zz", pat, &n, &mask, &flags), "bad hex");
    CHECK(!smssearch_parse(SMSSEARCH_HEX, "  ", pat, &n, &mask, &flags), "empty hex");
    CHECK(smssearch_parse(SMSSEARCH_TEXT, "Ab", pat, &n, &mask, &flags) && n == 2 &&
          pat[0] == 'A' && (flags & SMSDEBUG_SEARCH_NOCASE), "text");

    smswatch_clear();
    CHECK(smswatch_add("  $C010 ") == 0 && smswatch_add("score") == 1, "add");
    CHECK(smswatch_add("   ") == -1, "empty add refused");
    CHECK(smswatch_count() == 2 && !strcmp(smswatch_text(0), "$C010"), "text trimmed");
    {
        unsigned g = smswatch_generation();
        smswatch_remove(0);
        CHECK(smswatch_count() == 1 && !strcmp(smswatch_text(0), "score"), "remove");
        CHECK(smswatch_generation() != g, "generation bumps");
    }
    smswatch_clear();
    CHECK(smswatch_count() == 0, "clear");

    CHECK(smsdebug_trace_match(&t, SMSDEBUG_TRACE_ALL, 0, 0), "all");
    CHECK(smsdebug_trace_match(&t, SMSDEBUG_TRACE_BRANCHES, 0, 0), "CALL is a branch");
    CHECK(!smsdebug_trace_match(&u, SMSDEBUG_TRACE_BRANCHES, 0, 0), "NOP is not");
    CHECK(smsdebug_trace_match(&t, SMSDEBUG_TRACE_RANGE, 0x8000, 0x80FF), "in range");
    CHECK(!smsdebug_trace_match(&u, SMSDEBUG_TRACE_RANGE, 0x8000, 0x80FF), "out of range");
    CHECK(smsdebug_trace_match(&t, SMSDEBUG_TRACE_RANGE, 0x80FF, 0x8000), "range swaps");
    smsdebug_trace_format(&t, buf, sizeof buf);
    CHECK(!strncmp(buf, "8033  CALL $8050", 16), "trace format");

    {
        /* the other Z80 branches: JP (HL), RST, RETI, DJNZ, JR NZ, RET Z; and plain loads */
        static const struct { uint8_t b[4]; int branch; const char *what; } cases[] = {
            { { 0xE9 }, 1, "JP (HL)" }, { { 0xFF }, 1, "RST 38h" },
            { { 0xED, 0x4D }, 1, "RETI" }, { { 0xED, 0x45 }, 1, "RETN" },
            { { 0x10, 0xFE }, 1, "DJNZ" }, { { 0x20, 0x05 }, 1, "JR NZ" },
            { { 0xC8 }, 1, "RET Z" }, { { 0xC3, 0x00, 0x10 }, 1, "JP nn" },
            { { 0x3E, 0x05 }, 0, "LD A,n" }, { { 0xED, 0xB0 }, 0, "LDIR" },
        };
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            smsdebug_trace e = { .pc = 0x100, .len = 1 };
            memcpy(e.bytes, cases[i].b, 4);
            CHECK(!!smsdebug_trace_match(&e, SMSDEBUG_TRACE_BRANCHES, 0, 0) == cases[i].branch,
                  cases[i].what);
        }
    }
    {
        smsdebug_breakpoint bp;
        memset(&bp, 0, sizeof bp);
        bp.type = SMSDEBUG_BP_VRAM; bp.start = 0x1800; bp.end = 0x1AFF;
        snprintf(bp.condition, sizeof bp.condition, "value != old");
        smsbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "VRAM write  $1800-$1AFF  if value != old"), "VRAM bp description");
        bp.type = SMSDEBUG_BP_CRAM; bp.start = bp.end = 5; bp.condition[0] = 0;
        smsbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "CRAM write  5"), "CRAM bp description");
        bp.type = SMSDEBUG_BP_OUT | SMSDEBUG_BP_IN; bp.start = bp.end = 0xBE;
        smsbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "In/Out  $BE"), "I/O bp description");
        bp.type = SMSDEBUG_BP_EXEC | SMSDEBUG_BP_WRITE; bp.start = bp.end = 0xC010;
        smsbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Exec/Write  $C010"), "memory bp description");
    }

    printf(fails ? "memtools_test: %d failed\n" : "memtools_test: ok\n", fails);
    return fails != 0;
}
