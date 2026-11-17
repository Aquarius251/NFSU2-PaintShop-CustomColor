#define _CRT_SECURE_NO_WARNINGS
/* NFSU2_CustomColor.asi - any RGB colour for every colour slot of the player's car (v1, 2026-11-15).
   F4 opens a colour picker (drawn through NFSU2_HDToggle.asi's draw callbacks). Colours are kept per car type and per
   slot in NFSU2_CustomColor.txt and put on at run time; the game data and the save game stay untouched.

   Game facts (SPEED2.EXE v1.2, static RE + memory reads, see side-session-notes/.../customcolor.md):
     - RideInfo: +0 car type (CarTypeInfo table [0x8a1ccc], 0x890 each, name at +0), +0x590 CarPart*[170] by slot id.
       CarPart = 14 bytes: +0 name hash, +5 level bits, +0xa u16 attribute list index.
     - 0x60fef0 CarPart::GetAttribute(this, nameHash, prev) ret 8 -> {hash, value} (thiscall).
     - 0x61d790 CompositeSkin(ride) cdecl: body paint (slot 0x3f) RED/GREEN/BLUE/GLOSS, swatches 0x44-0x48,0x4b,0x4c,
       rims/spinners (0x61de90, slots 0x49/0x4a), vinyl colours 0x4d-0x5c - all read through 0x60fef0.
     - CarRenderInfo (cri): +4 RideInfo*. Neon groups g=0..3 (slots 0x9a NEON, 0x9b ENGINE, 0x9c CABIN, 0x9d TRUNK):
       base RGBA bytes at +0x34c+g*12, delta (colour2 - colour) at +0x350+g*12, animated output +0x358+g*12.
       0x61fc20 NeonColour(this=cri, part, base*, delta*) ret 0xc. Headlight bulb colour (slot 0x9f) bytes R,G,B at +0x380.
     - 0x615a10 = per-car neon draw (thiscall cri), called from the world and front-end car renderers.
     - HUD colours (slots 0xa4 backing, 0xa5 needle, 0xa6 characters) read from the FE ride 0x8392c0 by 0x50f650 into
       globals: backing R/G/B 0x839bfc/0x839bf8/0x839bf4, characters 0x839c0c/08/04, needle 0x839c1c/18/14.
     - Front end flow [0x8654a4] == 3, in a drive == 6. Player car = [[0x8900b4]+4], its ride = [car+0x10].
*/
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

#define FLOW        0x8654a4
#define FE_RIDE     0x8392c0
#define CARTYPES    0x8a1ccc
#define P_LIST      0x8900b4
#define WORLD_RIDE  0x890170
#define RACE_SETUP  0x89e7a0
#define A_ATTR      0x60fef0
#define A_COMPOSITE 0x61d790
#define A_NEONDRAW  0x615a10
/* neon glow effects (wide glow, F9 neon, floor pool) moved to NFSU2_Neon.asi */           /* neon style in the exe right now: 1 = wide (ours), 0 = stock */
#define A_NEONCOL   0x61fc20

static char g_dir[MAX_PATH], g_logp[MAX_PATH], g_store[MAX_PATH];
static int g_enabled = 1, g_log = 1, g_key = VK_F4;
static void clog(const char *f, ...)
{
    FILE *o; va_list a; SYSTEMTIME t;
    if (!g_log) return;
    o = fopen(g_logp, "a"); if (!o) return;
    GetLocalTime(&t); fprintf(o, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(a, f); vfprintf(o, f, a); va_end(a); fputc('\n', o); fclose(o);
}
static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION m;
    if ((DWORD)p < 0x10000) return 0;
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (BYTE *)p + n <= (BYTE *)m.BaseAddress + m.RegionSize;
}
static DWORD bin_hash(const char *s) { DWORD h = 0xFFFFFFFF; for (; *s; s++) h = h * 33 + (BYTE)*s; return h; }
/* custom neon is a career reward: unlocked once the ending scene (EndingNis01, played after the final career race) starts,
   remembered in NFSU2_CustomColor_unlocks.txt; ini NeonCustomAlwaysUnlocked=1 unlocks it for testing */
static int g_neonUnlocked; static char g_unlockFile[MAX_PATH];
static DWORD H_RED, H_GREEN, H_BLUE, H_GLOSS;

/* ---------------- colour slots ---------------- */
enum { K_PAINT, K_NEON, K_LIGHT, K_HUD, K_SHINE, K_PURGE, K_GLOSS };
#define GLOSS_SLOT 0x23f           /* pseudo slot: body paint gloss 0..255 (skin alpha = reflection strength), kept in rgb */
#define PURGE_HOOD 0x1a2           /* pseudo slots on the NOS_PURGE part (0xa2): hood stream / wheel stream */
#define PURGE_WHEEL 0x2a2
#define PURGE_HOOD2 0x3a2          /* colour 2 of each stream (the stock purges are two-tone, e.g. magenta -> blue) */
#define PURGE_WHEEL2 0x4a2
#define SHINE_SLOT 0x13f            /* pseudo slot: the body paint edge / glancing-angle colour (same part as BASE_PAINT) */
/* shine slot of a paint slot (0x100 | slot) - body and the parts with their own light material; 0 = none */
static int shine_of(int slot)
{
    switch (slot & 0xFF) { case 0x3f: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4b: case 0x4c: return 0x100 | (slot & 0xFF); }
    return 0;
}
typedef struct { int slot; const char *name, *label; int kind, arg; } Slot;
static const Slot SLOTS[] = {
    { 0x3f, "BASE_PAINT",     "Body paint",       K_PAINT, 0 },
    { SHINE_SLOT, "PAINT_SHINE", "Body shine",       K_SHINE, 0 },
    { GLOSS_SLOT, "PAINT_GLOSS", "Body gloss",       K_GLOSS, 0 },
    { 0x145, "PAINT_SPOILER_SHINE", "Spoiler shine",     K_SHINE, 0 },
    { 0x14b, "PAINT_ROOF_SHINE",    "Roof scoop shine",  K_SHINE, 0 },
    { 0x14c, "PAINT_MIRROR_SHINE",  "Mirror shine",      K_SHINE, 0 },
    { 0x149, "PAINT_RIM_SHINE",     "Rim shine",         K_SHINE, 0 },
    { 0x146, "PAINT_BRAKE_SHINE",   "Caliper shine",     K_SHINE, 0 },
    { 0x147, "PAINT_EXHAUST_SHINE", "Exhaust tip shine", K_SHINE, 0 },
    { 0x148, "PAINT_AUDIO_SHINE",   "Trunk audio shine", K_SHINE, 0 },
    { 0x45, "PAINT_SPOILER",  "Spoiler paint",        K_PAINT, 0 },
    { 0x4b, "PAINT_ROOF",     "Roof scoop paint", K_PAINT, 0 },
    { 0x4c, "PAINT_MIRROR",   "Mirror paint",     K_PAINT, 0 },
    { 0x49, "PAINT_RIM",      "Rim paint",        K_PAINT, 0 },
    { 0x4a, "PAINT_SPINNER",  "Spinner paint",    K_PAINT, 0 },
    { 0x46, "PAINT_BRAKE",    "Caliper paint",    K_PAINT, 0 },
    { 0x44, "PAINT_ENGINE",   "Engine paint",     K_PAINT, 0 },
    { 0x47, "PAINT_EXHAUST",  "Exhaust tip paint",K_PAINT, 0 },
    { 0x48, "PAINT_AUDIO",    "Trunk audio paint",K_PAINT, 0 },
    { 0x4d, "VINYL_COLOUR0_0","Vinyl colour 1",   K_PAINT, 0 },
    { 0x4e, "VINYL_COLOUR0_1","Vinyl colour 2",   K_PAINT, 0 },
    { 0x4f, "VINYL_COLOUR0_2","Vinyl colour 3",   K_PAINT, 0 },
    { 0x50, "VINYL_COLOUR0_3","Vinyl colour 4",   K_PAINT, 0 },
    { 0x51, "VINYL_COLOUR1_0","Vinyl 2 colour 1", K_PAINT, 0 },
    { 0x52, "VINYL_COLOUR1_1","Vinyl 2 colour 2", K_PAINT, 0 },
    { 0x53, "VINYL_COLOUR1_2","Vinyl 2 colour 3", K_PAINT, 0 },
    { 0x54, "VINYL_COLOUR1_3","Vinyl 2 colour 4", K_PAINT, 0 },
    { 0x55, "VINYL_COLOUR2_0","Vinyl 3 colour 1", K_PAINT, 0 },
    { 0x56, "VINYL_COLOUR2_1","Vinyl 3 colour 2", K_PAINT, 0 },
    { 0x57, "VINYL_COLOUR2_2","Vinyl 3 colour 3", K_PAINT, 0 },
    { 0x58, "VINYL_COLOUR2_3","Vinyl 3 colour 4", K_PAINT, 0 },
    { 0x59, "VINYL_COLOUR3_0","Vinyl 4 colour 1", K_PAINT, 0 },
    { 0x5a, "VINYL_COLOUR3_1","Vinyl 4 colour 2", K_PAINT, 0 },
    { 0x5b, "VINYL_COLOUR3_2","Vinyl 4 colour 3", K_PAINT, 0 },
    { 0x5c, "VINYL_COLOUR3_3","Vinyl 4 colour 4", K_PAINT, 0 },
    /* D8: decal colour per decal slot (pseudo slot 0x200 | the decal texture slot 0x61..0x98) */
    { 0x261, "DECAL_HOOD_TEX0_COLOUR", "Decal colour (hood, slot 1)", K_PAINT, 0 },
    { 0x262, "DECAL_HOOD_TEX1_COLOUR", "Decal colour (hood, slot 2)", K_PAINT, 0 },
    { 0x263, "DECAL_HOOD_TEX2_COLOUR", "Decal colour (hood, slot 3)", K_PAINT, 0 },
    { 0x264, "DECAL_HOOD_TEX3_COLOUR", "Decal colour (hood, slot 4)", K_PAINT, 0 },
    { 0x265, "DECAL_HOOD_TEX4_COLOUR", "Decal colour (hood, slot 5)", K_PAINT, 0 },
    { 0x266, "DECAL_HOOD_TEX5_COLOUR", "Decal colour (hood, slot 6)", K_PAINT, 0 },
    { 0x267, "DECAL_HOOD_TEX6_COLOUR", "Decal colour (hood, slot 7)", K_PAINT, 0 },
    { 0x268, "DECAL_HOOD_TEX7_COLOUR", "Decal colour (hood, slot 8)", K_PAINT, 0 },
    { 0x269, "DECAL_FRONT_WINDOW_TEX0_COLOUR", "Decal colour (windshield, slot 1)", K_PAINT, 0 },
    { 0x26a, "DECAL_FRONT_WINDOW_TEX1_COLOUR", "Decal colour (windshield, slot 2)", K_PAINT, 0 },
    { 0x26b, "DECAL_FRONT_WINDOW_TEX2_COLOUR", "Decal colour (windshield, slot 3)", K_PAINT, 0 },
    { 0x26c, "DECAL_FRONT_WINDOW_TEX3_COLOUR", "Decal colour (windshield, slot 4)", K_PAINT, 0 },
    { 0x26d, "DECAL_FRONT_WINDOW_TEX4_COLOUR", "Decal colour (windshield, slot 5)", K_PAINT, 0 },
    { 0x26e, "DECAL_FRONT_WINDOW_TEX5_COLOUR", "Decal colour (windshield, slot 6)", K_PAINT, 0 },
    { 0x26f, "DECAL_FRONT_WINDOW_TEX6_COLOUR", "Decal colour (windshield, slot 7)", K_PAINT, 0 },
    { 0x270, "DECAL_FRONT_WINDOW_TEX7_COLOUR", "Decal colour (windshield, slot 8)", K_PAINT, 0 },
    { 0x271, "DECAL_REAR_WINDOW_TEX0_COLOUR", "Decal colour (rear window, slot 1)", K_PAINT, 0 },
    { 0x272, "DECAL_REAR_WINDOW_TEX1_COLOUR", "Decal colour (rear window, slot 2)", K_PAINT, 0 },
    { 0x273, "DECAL_REAR_WINDOW_TEX2_COLOUR", "Decal colour (rear window, slot 3)", K_PAINT, 0 },
    { 0x274, "DECAL_REAR_WINDOW_TEX3_COLOUR", "Decal colour (rear window, slot 4)", K_PAINT, 0 },
    { 0x275, "DECAL_REAR_WINDOW_TEX4_COLOUR", "Decal colour (rear window, slot 5)", K_PAINT, 0 },
    { 0x276, "DECAL_REAR_WINDOW_TEX5_COLOUR", "Decal colour (rear window, slot 6)", K_PAINT, 0 },
    { 0x277, "DECAL_REAR_WINDOW_TEX6_COLOUR", "Decal colour (rear window, slot 7)", K_PAINT, 0 },
    { 0x278, "DECAL_REAR_WINDOW_TEX7_COLOUR", "Decal colour (rear window, slot 8)", K_PAINT, 0 },
    { 0x279, "DECAL_LEFT_DOOR_TEX0_COLOUR", "Decal colour (left door, slot 1)", K_PAINT, 0 },
    { 0x27a, "DECAL_LEFT_DOOR_TEX1_COLOUR", "Decal colour (left door, slot 2)", K_PAINT, 0 },
    { 0x27b, "DECAL_LEFT_DOOR_TEX2_COLOUR", "Decal colour (left door, slot 3)", K_PAINT, 0 },
    { 0x27c, "DECAL_LEFT_DOOR_TEX3_COLOUR", "Decal colour (left door, slot 4)", K_PAINT, 0 },
    { 0x27d, "DECAL_LEFT_DOOR_TEX4_COLOUR", "Decal colour (left door, slot 5)", K_PAINT, 0 },
    { 0x27e, "DECAL_LEFT_DOOR_TEX5_COLOUR", "Decal colour (left door, slot 6)", K_PAINT, 0 },
    { 0x27f, "DECAL_LEFT_DOOR_TEX6_COLOUR", "Decal colour (left door, slot 7)", K_PAINT, 0 },
    { 0x280, "DECAL_LEFT_DOOR_TEX7_COLOUR", "Decal colour (left door, slot 8)", K_PAINT, 0 },
    { 0x281, "DECAL_RIGHT_DOOR_TEX0_COLOUR", "Decal colour (right door, slot 1)", K_PAINT, 0 },
    { 0x282, "DECAL_RIGHT_DOOR_TEX1_COLOUR", "Decal colour (right door, slot 2)", K_PAINT, 0 },
    { 0x283, "DECAL_RIGHT_DOOR_TEX2_COLOUR", "Decal colour (right door, slot 3)", K_PAINT, 0 },
    { 0x284, "DECAL_RIGHT_DOOR_TEX3_COLOUR", "Decal colour (right door, slot 4)", K_PAINT, 0 },
    { 0x285, "DECAL_RIGHT_DOOR_TEX4_COLOUR", "Decal colour (right door, slot 5)", K_PAINT, 0 },
    { 0x286, "DECAL_RIGHT_DOOR_TEX5_COLOUR", "Decal colour (right door, slot 6)", K_PAINT, 0 },
    { 0x287, "DECAL_RIGHT_DOOR_TEX6_COLOUR", "Decal colour (right door, slot 7)", K_PAINT, 0 },
    { 0x288, "DECAL_RIGHT_DOOR_TEX7_COLOUR", "Decal colour (right door, slot 8)", K_PAINT, 0 },
    { 0x289, "DECAL_LEFT_QUARTER_TEX0_COLOUR", "Decal colour (left quarter, slot 1)", K_PAINT, 0 },
    { 0x28a, "DECAL_LEFT_QUARTER_TEX1_COLOUR", "Decal colour (left quarter, slot 2)", K_PAINT, 0 },
    { 0x28b, "DECAL_LEFT_QUARTER_TEX2_COLOUR", "Decal colour (left quarter, slot 3)", K_PAINT, 0 },
    { 0x28c, "DECAL_LEFT_QUARTER_TEX3_COLOUR", "Decal colour (left quarter, slot 4)", K_PAINT, 0 },
    { 0x28d, "DECAL_LEFT_QUARTER_TEX4_COLOUR", "Decal colour (left quarter, slot 5)", K_PAINT, 0 },
    { 0x28e, "DECAL_LEFT_QUARTER_TEX5_COLOUR", "Decal colour (left quarter, slot 6)", K_PAINT, 0 },
    { 0x28f, "DECAL_LEFT_QUARTER_TEX6_COLOUR", "Decal colour (left quarter, slot 7)", K_PAINT, 0 },
    { 0x290, "DECAL_LEFT_QUARTER_TEX7_COLOUR", "Decal colour (left quarter, slot 8)", K_PAINT, 0 },
    { 0x291, "DECAL_RIGHT_QUARTER_TEX0_COLOUR", "Decal colour (right quarter, slot 1)", K_PAINT, 0 },
    { 0x292, "DECAL_RIGHT_QUARTER_TEX1_COLOUR", "Decal colour (right quarter, slot 2)", K_PAINT, 0 },
    { 0x293, "DECAL_RIGHT_QUARTER_TEX2_COLOUR", "Decal colour (right quarter, slot 3)", K_PAINT, 0 },
    { 0x294, "DECAL_RIGHT_QUARTER_TEX3_COLOUR", "Decal colour (right quarter, slot 4)", K_PAINT, 0 },
    { 0x295, "DECAL_RIGHT_QUARTER_TEX4_COLOUR", "Decal colour (right quarter, slot 5)", K_PAINT, 0 },
    { 0x296, "DECAL_RIGHT_QUARTER_TEX5_COLOUR", "Decal colour (right quarter, slot 6)", K_PAINT, 0 },
    { 0x297, "DECAL_RIGHT_QUARTER_TEX6_COLOUR", "Decal colour (right quarter, slot 7)", K_PAINT, 0 },
    { 0x298, "DECAL_RIGHT_QUARTER_TEX7_COLOUR", "Decal colour (right quarter, slot 8)", K_PAINT, 0 },
    { 0x33f, "PAINT_SHINE_AMOUNT",         "Body shine amount",        K_GLOSS, 0 },   /* D9: how much of the shine colour */
    { 0x345, "PAINT_SPOILER_SHINE_AMOUNT", "Spoiler shine amount",     K_GLOSS, 0 },
    { 0x346, "PAINT_BRAKE_SHINE_AMOUNT",   "Caliper shine amount",     K_GLOSS, 0 },
    { 0x347, "PAINT_EXHAUST_SHINE_AMOUNT", "Exhaust tip shine amount", K_GLOSS, 0 },
    { 0x348, "PAINT_AUDIO_SHINE_AMOUNT",   "Trunk audio shine amount", K_GLOSS, 0 },
    { 0x349, "PAINT_RIM_SHINE_AMOUNT",     "Rim shine amount",         K_GLOSS, 0 },
    { 0x34b, "PAINT_ROOF_SHINE_AMOUNT",    "Roof scoop shine amount",  K_GLOSS, 0 },
    { 0x34c, "PAINT_MIRROR_SHINE_AMOUNT",  "Mirror shine amount",      K_GLOSS, 0 },
    { 0x53f, "PAINT_SATURATION",           "Body saturation",          K_GLOSS, 0 },   /* v1.1: colour saturation 0-200 % */
    { 0x545, "PAINT_SPOILER_SATURATION",   "Spoiler saturation",       K_GLOSS, 0 },
    { 0x546, "PAINT_BRAKE_SATURATION",     "Caliper saturation",       K_GLOSS, 0 },
    { 0x547, "PAINT_EXHAUST_SATURATION",   "Exhaust tip saturation",   K_GLOSS, 0 },
    { 0x548, "PAINT_AUDIO_SATURATION",     "Trunk audio saturation",   K_GLOSS, 0 },
    { 0x549, "PAINT_RIM_SATURATION",       "Rim saturation",           K_GLOSS, 0 },
    { 0x54b, "PAINT_ROOF_SATURATION",      "Roof scoop saturation",    K_GLOSS, 0 },
    { 0x54c, "PAINT_MIRROR_SATURATION",    "Mirror saturation",        K_GLOSS, 0 },
    { 0x240, "VINYL_PICTURE0", "Picture vinyl colour",   K_PAINT, 0 },     /* tint of a picture vinyl on layer 1..4 (V1) */
    { 0x241, "VINYL_PICTURE1", "Picture vinyl 2 colour", K_PAINT, 0 },
    { 0x242, "VINYL_PICTURE2", "Picture vinyl 3 colour", K_PAINT, 0 },
    { 0x243, "VINYL_PICTURE3", "Picture vinyl 4 colour", K_PAINT, 0 },
    { 0x9a, "NEON",           "Underglow neon",   K_NEON, 0 },
    { 0x9b, "NEON_ENGINE",    "Engine neon",      K_NEON, 1 },
    { 0x9c, "NEON_CABIN",     "Cabin neon",       K_NEON, 2 },
    { 0x9d, "NEON_TRUNK",     "Trunk neon",       K_NEON, 3 },
    { 0x9f, "HEADLIGHT_BULB", "Headlights",       K_LIGHT, 0 },
    { 0xa4, "HUD_BACKING_COLOUR",   "HUD backing",    K_HUD, 0 },
    { 0xa5, "HUD_NEEDLE_COLOUR",    "HUD needle",     K_HUD, 2 },
    { 0xa6, "HUD_CHARACTER_COLOUR", "HUD numbers",    K_HUD, 1 },
    { 0x1a4, "HUD_NITRO_COLOUR",    "HUD nitro bar",      K_HUD, 3 },
    { 0x2a4, "HUD_REDLINE_COLOUR",  "HUD redline",        K_HUD, 3 },
    { 0x1a5, "HUD_SPEED_COLOUR",    "HUD speed + gear",   K_HUD, 3 },
    { 0x2a5, "HUD_RING_COLOUR",     "HUD ring + gear box", K_HUD, 3 },
    { PURGE_HOOD,  "PURGE_HOOD",  "Purge (hood)",   K_PURGE, 0 },
    { PURGE_WHEEL, "PURGE_WHEEL", "Purge (wheels)", K_PURGE, 1 },
    { PURGE_HOOD2,  "PURGE_HOOD2",  "Purge (hood) colour 2",   K_PURGE, 0 },
    { PURGE_WHEEL2, "PURGE_WHEEL2", "Purge (wheels) colour 2", K_PURGE, 1 },
};
#define NSLOT (int)(sizeof SLOTS / sizeof SLOTS[0])
static int g_kindOn[7] = { 1, 1, 1, 1, 1, 1, 1 };

/* ---------------- stored colours: per car type + slot ---------------- */
typedef struct { char car[32]; int slot; DWORD part; DWORD rgb; int seq; } Entry;   /* seq: order the colour was set in (newest = highest) */   /* part = name hash of the game part it was set on */
#define MAXE 1024
static Entry g_e[MAXE]; static int g_ne, g_seq;
static volatile LONG g_ver, g_purgeVer;            /* bumped when a paint slot (composited skin) changes */
static CRITICAL_SECTION g_cs;
static Entry *find_e(const char *car, int slot)
{
    int i;
    for (i = 0; i < g_ne; i++) if (g_e[i].slot == slot && !_stricmp(g_e[i].car, car)) return &g_e[i];
    return NULL;
}
/* Vinyl colours belong to the vinyl, not to the layer slot: the game can put the same vinyl on another layer (preview vs
   installed), so a vinyl colour is stored as (pseudo slot VINYLC + colour 1..4, part = the vinyl's name hash).
   Ride slots 0x4d..0x5c = layer L (0x40 + L) colour c: slot 0x4d + 4L + c. */
static BYTE *part_of_(BYTE *ride, int slot) { return *(BYTE **)(ride + 0x590 + (slot & 0xFF) * 4); }
static DWORD part_hash_(BYTE *p) { return p ? *(DWORD *)p : 0; }
#define VINYLC 0x300
#define IS_VKEY(k) ((k) >= VINYLC && (k) <= VINYLC + 3)
static int is_vinyl_slot(int slot) { return slot >= 0x4d && slot <= 0x5c; }
static DWORD vinyl_of(BYTE *ride, int slot) { BYTE *v = *(BYTE **)(ride + 0x590 + (0x40 + (slot - 0x4d) / 4) * 4); return v ? *(DWORD *)v : 0; }
/* E8b RULE (user 17.11): "SLOT for vinyl or decal always hold selected color, custom or default ... just color change will
   change it". Vinyl colours (0x4d..0x5c = layer L colour c) and picture tints (0x240..0x243) are keyed by the SLOT, whatever
   vinyl is in it; old VINYLCn lines (per vinyl) are converted to the slot the first time that vinyl is seen on a layer. */
static DWORD g_dirtyAt; static int g_dirty;               /* (tentative: defined again further down) */
static int key_of(BYTE *ride, int slot, DWORD *part)     /* -> stored slot (always the slot itself now) */
{
    (void)ride; *part = 0; return slot;
}
/* E3: a picture vinyl colour (slots 0x240..0x243) belongs to one picture - the vinyl part's name hash - on whatever layer
   it sits. A picture without its own colour shows its own colours (Default). In the vinyl list the picker edits the
   picture the list has selected (the preview car), not the one installed on the car. */
#define IS_PIC(s) ((s) >= 0x240 && (s) <= 0x243)
/* E7: in the vinyl list ONE browsing colour shows on every picture vinyl (pick a colour, then look for a vinyl to go with
   it). Default = the pictures as drawn. Install saves the browsing colour with the installed picture (shop cart: the
   preview car is 0x8389d0, the installed car FE ride 0x8392c0 only changes on Install). The list opens with the installed
   picture's saved colour as the browsing colour. Outside the list every picture shows its own saved colour. */
#define PIC_BROWSE() (0)                /* E8b: replaced by the slot rule (the slot's colour shows on every vinyl browsed) */
static DWORD g_browseCol, g_instPic; static int g_browseOn;
static void pic_browse_set(int on, DWORD rgb); static void pic_browse_enter(void); static void pic_install_watch(void);
#define NVPAL 11                    /* E6: picture vinyl preset row (see vinyl_palette) */
static DWORD g_vpal[NVPAL]; static int g_nvpal = -1; static float g_vpDef[4], g_vpR[NVPAL][4]; static int g_vpOn;
static BYTE *g_browser; static int browser_slot(void);
static BYTE *shown_pic(void);
static int g_vinylBrowse;          /* the parts browser was opened from Choose Vinyl Layer: it lists vinyls */
static BYTE *list_part(void) { return *(int *)FLOW == 3 && g_vinylBrowse ? shown_pic() : NULL; }   /* the picture on the car on the turntable */
static BYTE *pic_edit_part(BYTE *ride, int slot) { BYTE *lp = list_part(); return lp ? lp : ride ? part_of_(ride, slot) : NULL; }
static Entry *find_pic(const char *car, DWORD ph)
{
    int i; if (!ph) return NULL;
    for (i = 0; i < g_ne; i++) if (IS_PIC(g_e[i].slot) && g_e[i].part == ph && !_stricmp(g_e[i].car, car)) return &g_e[i];
    return NULL;
}
static Entry *find_r(const char *car, BYTE *ride, int slot)
{
    DWORD vp; int k = key_of(ride, slot, &vp), i;
    if (IS_PIC(slot)) return find_e(car, slot);
    if (!IS_VKEY(k)) return find_e(car, slot);
    if (!vp) return NULL;
    for (i = 0; i < g_ne; i++) if (g_e[i].slot == k && g_e[i].part == vp && !_stricmp(g_e[i].car, car)) return &g_e[i];
    return NULL;
}
/* the newest vinyl colour c set on this car: shown on vinyls that have no colour of their own (previewing vinyls in the
   list, a newly installed vinyl) -> the vinyl keeps the player's colour instead of each vinyl's stock colours */
static Entry *newest_vinyl(const char *car, int k)
{
    Entry *b = NULL; int i;
    for (i = 0; i < g_ne; i++) if (g_e[i].slot == k && !_stricmp(g_e[i].car, car) && (!b || g_e[i].seq > b->seq)) b = &g_e[i];
    return b;
}
static Entry *make_r(const char *car, BYTE *ride, int slot)        /* find or add (caller holds g_cs) */
{
    Entry *e = find_r(car, ride, slot); DWORD vp; int k = key_of(ride, slot, &vp);
    if (e) return e;
    if (IS_VKEY(k) && !vp) return NULL;
    (void)pic_edit_part;
    if (g_ne >= MAXE) return NULL;
    e = &g_e[g_ne++]; memset(e, 0, sizeof *e); strncpy(e->car, car, 31); e->slot = k; e->part = vp;
    return e;
}
static void load_store(void)
{
    FILE *f = fopen(g_store, "r"); char ln[256], car[64], sn[64]; unsigned part, rgb; int i;
    if (!f) { clog("no %s yet (no custom colours saved)", g_store); return; }
    while (fgets(ln, sizeof ln, f)) {
        if (ln[0] == ';' || ln[0] == '#') continue;
        if (sscanf(ln, "%63s %63s %x %x", car, sn, &part, &rgb) != 4) continue;
        if (!_strnicmp(sn, "VINYLC", 6) && sn[6] >= '1' && sn[6] <= '4' && g_ne < MAXE) {
            strncpy(g_e[g_ne].car, car, 31); g_e[g_ne].slot = VINYLC + sn[6] - '1'; g_e[g_ne].part = part; g_e[g_ne].rgb = rgb & 0xFFFFFF; g_e[g_ne].seq = ++g_seq; g_ne++;
            continue;
        }
        if (!_strnicmp(sn, "SAT_", 4) && g_ne < MAXE) {          /* E13: saturation of a vinyl / picture / decal slot */
            unsigned sb; Entry *ex;
            if (sscanf(sn + 4, "%x", &sb) == 1 && sb < 0x100) {
                if ((ex = find_e(car, 0x500 | sb))) { ex->rgb = rgb & 255; ex->seq = ++g_seq; continue; }
                strncpy(g_e[g_ne].car, car, 31); g_e[g_ne].slot = 0x500 | sb; g_e[g_ne].part = 0; g_e[g_ne].rgb = rgb & 255; g_e[g_ne].seq = ++g_seq; g_ne++;
            }
            continue;
        }
        for (i = 0; i < NSLOT; i++) if (!_stricmp(SLOTS[i].name, sn)) break;
        if (i == NSLOT || g_ne >= MAXE) continue;
        {   Entry *ex = find_e(car, SLOTS[i].slot);           /* (old per-picture tints: the last one set wins the slot) */
            if (ex && (IS_PIC(SLOTS[i].slot) || is_vinyl_slot(SLOTS[i].slot))) { ex->part = 0; ex->rgb = rgb & 0xFFFFFF; ex->seq = ++g_seq; continue; }
        }
        strncpy(g_e[g_ne].car, car, 31); g_e[g_ne].slot = SLOTS[i].slot; g_e[g_ne].part = (IS_PIC(SLOTS[i].slot) || is_vinyl_slot(SLOTS[i].slot)) ? 0 : part;
        g_e[g_ne].rgb = rgb & 0xFFFFFF; g_e[g_ne].seq = ++g_seq; g_ne++;
    }
    fclose(f);
    clog("loaded %d custom colours from %s", g_ne, g_store);
}
static void save_store(void)
{
    char tmp[MAX_PATH]; FILE *f; int i, k;
    sprintf(tmp, "%s.tmp", g_store);
    f = fopen(tmp, "w"); if (!f) { clog("cannot write %s", tmp); return; }
    fprintf(f, "; NFSU2_CustomColor - car type, slot, game part it was set on (name hash), colour RRGGBB. Written by the game.\n");
    {   /* oldest first: the line order is the order the colours were set in (newest vinyl colour = last line) */
        static Entry *ord[MAXE]; int n = 0, a, b;
        for (i = 0; i < g_ne; i++) ord[n++] = &g_e[i];
        for (a = 1; a < n; a++) { Entry *t = ord[a]; for (b = a; b > 0 && ord[b - 1]->seq > t->seq; b--) ord[b] = ord[b - 1]; ord[b] = t; }
        for (a = 0; a < n; a++) { Entry *e = ord[a];
            if (IS_VKEY(e->slot)) { fprintf(f, "%s VINYLC%d %08X %06X\n", e->car, e->slot - VINYLC + 1, e->part, e->rgb); continue; }
            for (k = 0; k < NSLOT; k++) if (SLOTS[k].slot == e->slot) break;
            if (k < NSLOT) fprintf(f, "%s %s %08X %06X\n", e->car, SLOTS[k].name, e->part, e->rgb);
            else if ((e->slot & 0xF00) == 0x500) fprintf(f, "%s SAT_%02X %08X %06X\n", e->car, e->slot & 0xFF, e->part, e->rgb);
        }
    }
    fclose(f);
    if (!MoveFileExA(tmp, g_store, MOVEFILE_REPLACE_EXISTING)) clog("cannot replace %s (%lu)", g_store, GetLastError());
}

/* ---------------- game helpers ---------------- */
static const char *car_name(BYTE *ride)
{
    static char nm[32]; BYTE *ct = *(BYTE **)CARTYPES; int t;
    if (!readable(ride, 4) || !ct) return NULL;
    t = *(int *)ride;
    if (t < 0 || t > 400 || !readable(ct + t * 0x890, 32)) return NULL;
    memcpy(nm, ct + t * 0x890, 31); nm[31] = 0;
    return nm[0] ? nm : NULL;
}
static BYTE *part_of(BYTE *ride, int slot) { return *(BYTE **)(ride + 0x590 + (slot & 0xFF) * 4); }   /* SHINE_SLOT -> BASE_PAINT part */
static DWORD part_hash(BYTE *p) { return p ? *(DWORD *)p : 0; }
static BYTE *player_ride(void)
{
    static BYTE *last; static DWORD t; BYTE *p, *car; DWORD now = GetTickCount();
    if (now - t < 100) return last;               /* cached: called for every car every frame */
    t = now; last = NULL;
    p = *(BYTE **)P_LIST;
    if (!readable(p, 8)) return NULL;
    car = *(BYTE **)(p + 4);
    if (!readable(car, 0x14)) return NULL;
    return last = *(BYTE **)(car + 0x10);
}
/* is this ride the player's own car? front end: the car type the player is customising; world: the player's car / human entry */
static BYTE *g_compMine;     /* ride being composited that was ours when the composite started (its part list gets our
                                stand-in colour parts during the composite, so the identical-parts test would fail mid-way) */
static int has_paint(BYTE *ride);
static int mine(BYTE *ride)
{
    int flow = *(int *)FLOW, i, n; BYTE *arr;
    if (!ride) return 0;
    if (ride == g_compMine) return 1;
    if (ride == (BYTE *)FE_RIDE) return 1;
    /* the player's free-roam car (fixed address). It is rebuilt and re-painted (body skin, rims) while the world loads after a
       shop / the garage - before the player list points at it - so it must count as ours then too, else it got the stock colours */
    if (ride == (BYTE *)WORLD_RIDE) return 1;
    /* the world load paints heap copies of the player's car (seen live: same car type, identical 170-part list as the FE ride;
       traffic / rivals differ) */
    if (readable(ride, 0x590 + 170 * 4) && *(int *)ride == *(int *)FE_RIDE && !memcmp(ride + 0x590, (BYTE *)FE_RIDE + 0x590, 170 * 4)) return 1;
    /* first world load after the game starts (QuickStart / straight to free roam): the menu ride may still hold another car
       (main menu background), so also accept copies of the player's free-roam ride */
    if (ride != (BYTE *)WORLD_RIDE && readable(ride, 0x590 + 170 * 4) && *(int *)ride == *(int *)WORLD_RIDE && *(DWORD *)((BYTE *)WORLD_RIDE + 0x590 + 0x3f * 4)
        && !memcmp(ride + 0x590, (BYTE *)WORLD_RIDE + 0x590, 170 * 4)) return 1;
    if (flow == 3) {
        if (*(int *)ride == *(int *)FE_RIDE) return 1;
        /* main menu right after the game starts: the menu ride is not filled yet (type -1), the car shown behind the menu is
           the player's own -> it got the stock colours until the garage / a shop was opened */
        return *(int *)FE_RIDE == -1 && has_paint(ride);
    }
    if (ride == player_ride()) return 1;
    arr = *(BYTE **)(RACE_SETUP + 0x70); n = *(int *)(RACE_SETUP + 0x74);
    if (readable(arr, 0x940) && n > 0 && n < 64)
        for (i = 0; i < n; i++) if (arr + i * 0x940 + 0x10 == ride) return *(int *)(arr + i * 0x940 + 4) == 1;
    return 0;
}
/* the custom colour for this ride + slot, if any and if the slot still has the part it was set on */
static int preset_view(int slot);     /* 1 = the shop is showing its own presets for this slot right now */
static int row_hides(int slot);       /* 1 = a game row (not CUSTOM) is selected in the list that has our CUSTOM row */
static int custom_for(BYTE *ride, int slot, DWORD *rgb)
{
    const char *car; Entry *e; BYTE *p; int ok = 0;
    if (!g_enabled || !mine(ride) || !(car = car_name(ride))) return 0;
    if (preset_view(slot) || row_hides(slot)) return 0;
    if (IS_PIC(slot) && PIC_BROWSE()) { if (!g_browseOn) return 0; *rgb = g_browseCol; return 1; }
    if (slot >= 0x9a && slot <= 0x9d && !g_neonUnlocked) return 0;
    p = part_of(ride, slot);
    EnterCriticalSection(&g_cs);
    e = find_r(car, ride, slot);
    if (!e && slot == (slot & 0xFF) && is_vinyl_slot(slot)) {        /* an old per-vinyl colour of the vinyl now in this slot */
        DWORD vp = vinyl_of(ride, slot); int k = VINYLC + (slot - 0x4d) % 4, i;
        for (i = 0; vp && i < g_ne; i++) if (g_e[i].slot == k && g_e[i].part == vp && !_stricmp(g_e[i].car, car)) break;
        if (vp && i < g_ne && g_ne < MAXE) {
            Entry old = g_e[i]; g_e[i] = g_e[--g_ne];
            e = &g_e[g_ne++]; *e = old; e->slot = slot; e->part = 0;
            g_dirty = 1; g_dirtyAt = GetTickCount();
            clog("vinyl colour: old colour %06lX of vinyl %08lx now belongs to slot %02x", e->rgb, vp, slot);
        }
    }
    /* the colour belongs to the part it was set on: a preset installed over it (mouse pick via the game's "Add this paint?"
       box, which never sends PAD_ACCEPT) changes the part -> the custom colour is no longer shown. Paint type switches inside
       the picker move the colour to the new part (move_custom), vinyl keys are matched by vinyl part in find_r. */
    /* picture vinyl colours (0x240..0x243): E3 - each picture its own colour (find_pic by the layer's vinyl part); a picture
       without one shows its own colours (the user's "Default") */
    if (e && 0 != part_hash(p) && (!e->part || e->part == part_hash(p) || is_vinyl_slot(slot & 0xFF) || (slot & 0xFF) >= 0x9a || (slot >= 0x240 && slot <= 0x243) || ((slot & 0xF00) == 0x200 && (slot & 0xFF) >= 0x61 && (slot & 0xFF) <= 0x98) || (slot & 0xF00) == 0x500)) { *rgb = e->rgb; ok = 1; }   /* E8: a decal SLOT keeps its colour for any decal put there */
    LeaveCriticalSection(&g_cs);
    return ok;
}

/* ---------------- code hooks ---------------- */
static BYTE *make_hook(DWORD at, const BYTE *expect, int n, void *to, void **out)
{
    BYTE *t; DWORD old; int i;
    if (memcmp((void *)at, expect, n)) {
        char hx[64] = ""; for (i = 0; i < n; i++) sprintf(hx + i * 3, "%02x ", ((BYTE *)at)[i]);
        clog("hook at %08lx: unexpected bytes %s- not hooked", at, hx);
        return NULL;
    }
    t = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, (void *)at, n); t[n] = 0xE9; *(DWORD *)(t + n + 1) = at + n - (DWORD)(t + n + 5);
    *out = t;                                   /* the hook may run as soon as the jump is in */
    VirtualProtect((void *)at, n, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE *)at = 0xE9; *(DWORD *)(at + 1) = (DWORD)to - (at + 5);
    for (i = 5; i < n; i++) ((BYTE *)at)[i] = 0x90;
    VirtualProtect((void *)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, n);
    return t;
}

/* fake parts: a copy of the real part (14 bytes) + custom RED/GREEN/BLUE answered by the attribute hook */
typedef struct { BYTE raw[16]; BYTE *orig; DWORD at[4][2]; DWORD pad; } Fake;
static Fake g_fakes[NSLOT];
static DWORD g_fLo, g_fHi;
typedef DWORD *(__fastcall *PFN_Attr)(void *part, void *edx, DWORD hash, DWORD *prev);
static PFN_Attr o_attr;
static DWORD *__cdecl fake_attr(Fake *f, DWORD hash, DWORD *prev)
{
    int k = hash == H_RED ? 0 : hash == H_GREEN ? 1 : hash == H_BLUE ? 2 : (hash == H_GLOSS && f->at[3][0]) ? 3 : -1;
    if (k >= 0) return prev ? NULL : f->at[k];
    return o_attr(f->orig, NULL, hash, prev);
}
/* E1 probe (log only): which colour-count attributes the shop reads for which part, and from where. NUMREMAPCOLOURS
   (0x6212682b) and NUMCOLOURS (0x564b8cb6) through GetAttribute; the compositor reads NUMCOLOURS from the table itself. */
static DWORD g_prb[300][3]; static int g_nprb;
static BYTE *g_nav;
static DWORD *__cdecl probe_attr(BYTE *part, DWORD hash, DWORD *prev, DWORD ret, DWORD ret2)
{
    DWORD *r = o_attr(part, NULL, hash, prev); int i;
    if (!prev) {
        if (ret == 0x61b50c) ret = ret2;                 /* E3: through the 0x61b500 helper -> log its caller */
        for (i = 0; i < g_nprb; i++) if (g_prb[i][0] == (DWORD)part && g_prb[i][1] == hash && g_prb[i][2] == ret) break;
        if (i == g_nprb && g_nprb < 300) {
            char *nm = NULL; g_prb[g_nprb][0] = (DWORD)part; g_prb[g_nprb][1] = hash; g_prb[g_nprb][2] = ret; g_nprb++;
            __try { nm = ((char *(__stdcall *)(BYTE *))0x50e530)(part); } __except (EXCEPTION_EXECUTE_HANDLER) { nm = NULL; }
            clog("attr probe: %s %s = %ld (caller %08lx, flow %d, screen %s)", nm && readable(nm, 4) ? nm : "?", hash == 0x6212682b ? "NUMREMAPCOLOURS" : "NUMCOLOURS",
                 r ? (long)r[1] : -1L, ret, *(int *)FLOW, g_nav && readable(g_nav, 0x5c) && readable(*(char **)(g_nav + 0x58), 4) ? *(char **)(g_nav + 0x58) : "?");
        }
    }
    return r;
}
__declspec(naked) static void h_attr(void)
{
    __asm {
        cmp ecx, g_fLo
        jb  orig
        cmp ecx, g_fHi
        jae orig
        mov eax, [esp + 8]
        push eax
        mov eax, [esp + 8]
        push eax
        push ecx
        call fake_attr
        add esp, 12
        ret 8
    orig:
#ifndef NFSU2CC_SHIP
        mov eax, [esp + 4]
        cmp eax, 0x6212682b
        je probe
        cmp eax, 0x564b8cb6
        je probe
        jmp dword ptr [o_attr]
    probe:
        push dword ptr [esp + 12]
        push dword ptr [esp + 4]
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        push ecx
        call probe_attr
        add esp, 20
        ret 8
#else
        jmp dword ptr [o_attr]
#endif
    }
}

typedef int (__cdecl *PFN_Comp)(BYTE *ride);
static PFN_Comp o_comp;
static long g_cComp, g_cCompCustom;
static DWORD game_rgb(BYTE *ride, int slot);
/* v1.1 SATURATION. Dark custom paints looked washed out: with a shine colour the body skin lifts every channel to >= 0x40
   (so the edges can turn into the shine colour), and deep red 900010 became 904040. The Saturation slider (0..200 %, stored
   0..255 at 0x500 | slot, 128 = 100 % = the look before) scales the colour's saturation, and above 100 % it lowers that
   lift to none at 200 % - deep colours stay deep. */
static void rgb_hsv(DWORD c, float *h, float *s, float *v); static DWORD hsv_rgb(float h, float s, float v);
static int custom_for(BYTE *ride, int slot, DWORD *rgb);
/* E13: vinyl colours (0x4d..0x5c), picture tints (0x240..0x243 -> 0x540..0x543) and decal slots (0x261..0x298 -> 0x561..
   0x598) get a saturation too: key 0x500 | (slot & 0xFF); paints keep their PAINT_..._SATURATION names, the rest is saved
   as "SAT_xx". */
static int sat_slot_ok(int slot)
{
    int b = slot & 0xFF;
    return (slot < 0x100 && (b == 0x3f || (b >= 0x45 && b <= 0x4c) || (b >= 0x4d && b <= 0x5c)))
        || (slot >= 0x240 && slot <= 0x243) || ((slot & 0xF00) == 0x200 && b >= 0x61 && b <= 0x98);
}
static float sat_amt(BYTE *ride, int slot)
{
    DWORD a; if (!custom_for(ride, 0x500 | (slot & 0xFF), &a)) return 1.f;
    return (a & 255) / 128.f;
}
static DWORD sat_rgb(DWORD c, float k)              /* saturation x k (value kept) */
{
    float h, s, v;
    if (k > 0.995f && k < 1.005f) return c & 0xFFFFFF;
    rgb_hsv(c & 0xFFFFFF, &h, &s, &v); s *= k; if (s > 1.f) s = 1.f; if (s < 0.f) s = 0.f;
    return hsv_rgb(h, s, v) & 0xFFFFFF;
}
static DWORD shine_skin2(DWORD base, float sat)     /* body skin with a shine: no channel below 0x40 (less above 100 % saturation) */
{
    DWORD r = (base >> 16) & 255, g = (base >> 8) & 255, b = base & 255, fl = 0x40;
    if (sat > 1.f) { float t = 2.f - sat; if (t < 0) t = 0; fl = (DWORD)(0x40 * t + .5f); }
    if (r < fl) r = fl; if (g < fl) g = fl; if (b < fl) b = fl;
    return (r << 16) | (g << 8) | b;
}
static DWORD shine_skin(DWORD base) { return shine_skin2(base, 1.f); }
/* D9 shine amount: stored 0..255, 128 = 100 % (the look before D9), 255 = 200 %. Up to 100 % the shine colour is mixed
   towards the base colour. Above 100 % the shine also goes INTO the paint: the skin (composited body colour) takes up to 85 %
   of the shine colour, so the shader can show it on edges and flat areas (on a blue skin a pink shine had no red to work
   with - the skin multiplies the light colour); the face keeps the base look (its light colour = base / skin, 8 % shine at
   200 %) and the edge limit doubles. Tuned on offline renders (screenshots/tint_check/shine_amount_variants.png, "C"). */
static float shine_amt(BYTE *ride, int slot)
{
    DWORD a; if (!custom_for(ride, 0x300 | (slot & 0xFF), &a)) return 1.f;
    return (a & 255) / 128.f;
}
static DWORD mix_rgb(DWORD a, DWORD b, float t)
{
    DWORD out = 0; int k; if (t < 0) t = 0; if (t > 1) t = 1;
    for (k = 16; k >= 0; k -= 8) out |= (DWORD)(((a >> k) & 255) * (1 - t) + ((b >> k) & 255) * t + .5f) << k;
    return out;
}
static int __cdecl h_comp(BYTE *ride)
{
    BYTE *saved[NSLOT]; int i, r, n = 0; DWORD rgb, tmpS;
    memset(saved, 0, sizeof saved);
    g_compMine = NULL;
    __try { if (readable(ride, 0x590 + 170 * 4) && mine(ride)) g_compMine = ride; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        if (g_enabled && g_kindOn[K_PAINT] && readable(ride, 0x590 + 170 * 4))
            for (i = 0; i < NSLOT; i++) {
                BYTE *p;
                if (SLOTS[i].kind != K_PAINT || (SLOTS[i].slot & 0xF00) == 0x200 || !(p = part_of(ride, SLOTS[i].slot)) || (DWORD)p - g_fLo < g_fHi - g_fLo) continue;
                if (!custom_for(ride, SLOTS[i].slot, &rgb)) {
                    DWORD sh;                                    /* shine / gloss / saturation without own base: the part's colour */
                    int so = shine_of(SLOTS[i].slot);
                    if (!((so && g_kindOn[K_SHINE] && custom_for(ride, so, &sh)) || (SLOTS[i].slot == 0x3f && custom_for(ride, GLOSS_SLOT, &sh))
                          || (SLOTS[i].slot < 0x100 && custom_for(ride, 0x500 | SLOTS[i].slot, &sh)))) continue;
                    rgb = game_rgb(ride, SLOTS[i].slot);
                }
                {   float sa = SLOTS[i].slot < 0x100 ? sat_amt(ride, SLOTS[i].slot) : 1.f;
                    rgb = sat_rgb(rgb, sa);
                    if (shine_of(SLOTS[i].slot) && g_kindOn[K_SHINE] && custom_for(ride, shine_of(SLOTS[i].slot), &tmpS)) {
                        float am = shine_amt(ride, SLOTS[i].slot);
                        tmpS = sat_rgb(tmpS, sa);
                        rgb = shine_skin2(am > 1.f ? mix_rgb(rgb, tmpS, 0.85f * (am - 1.f)) : rgb, sa);   /* > 100 %: shine into the skin */
                    }
                }
                memcpy(g_fakes[i].raw, p, 14); g_fakes[i].orig = p;
                g_fakes[i].at[0][0] = H_RED;   g_fakes[i].at[0][1] = (rgb >> 16) & 0xFF;
                g_fakes[i].at[1][0] = H_GREEN; g_fakes[i].at[1][1] = (rgb >> 8) & 0xFF;
                g_fakes[i].at[2][0] = H_BLUE;  g_fakes[i].at[2][1] = rgb & 0xFF;
                { DWORD gl; g_fakes[i].at[3][0] = 0;
                  if (SLOTS[i].slot == 0x3f && custom_for(ride, GLOSS_SLOT, &gl)) { g_fakes[i].at[3][0] = H_GLOSS; g_fakes[i].at[3][1] = gl & 0xFF; } }
                saved[i] = p; *(BYTE **)(ride + 0x590 + SLOTS[i].slot * 4) = g_fakes[i].raw; n++;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) { clog("composite: exception while setting colours"); }
    g_cComp++; if (n) g_cCompCustom++;
    __try { r = o_comp(ride); }
    __finally { for (i = 0; i < NSLOT; i++) if (saved[i]) *(BYTE **)(ride + 0x590 + SLOTS[i].slot * 4) = saved[i]; g_compMine = NULL; }
    if (g_cComp <= 40 || n || ride == (BYTE *)WORLD_RIDE) clog("composite ride %p (%s, flow %d): %d custom slot(s), result %d, mine %d, vinyl %08lx, FE type %d",
        ride, car_name(ride) ? car_name(ride) : "?", *(int *)FLOW, n, r, mine(ride), vinyl_of(ride, 0x4d), *(int *)FE_RIDE);
    return r;
}

/* Picture vinyls (full-colour images: ART_F_*, CONTEST_*, UNIQUE_*, TOP_HOOD_FLAMESKULL / MECHAMOTH) have no colour
   slots, so the game draws them as they are. V1: they get the player's newest vinyl colour 1 by a "colorize" (hue +
   saturation of the custom colour, lightness of each pixel: white stays white, black stays black).
   Where: the paint job (esp+0x6c in 0x61dd60, 0x1c8 bytes: +0 skin TI, +0x24 8 layers of 0x34, +0x1c4 count; layer +0
   hash, +4 colour TI (+0xc name, +0x44/+0x46 w/h), +0xc colour pixels ARGB, +0x20 tint flag) is complete at the call
   0x61dd8b (cache compare 0x611970). The layer's pixel pointer is pointed at a tinted copy there (the game's own pixels
   are never written; the job is a local, nothing to restore). The vinyl HD add-on owns the later call 0x61ddf0 and reads
   the job there, so it gets the tinted pixels too. Only the player's car (composite of a ride that is ours). */
#define A_VCACHECALL 0x61dd8b
static void hsv2rgb(float h, float s, float v, float *r, float *g, float *b); static void rgb_hsv(DWORD c, float *h, float *s, float *v);
#define A_VCACHE     0x611970
static int g_vTint = 1;
typedef struct { DWORD *src, *dst; int w, h; DWORD rgb, used; } VTint;
static VTint g_vt[24]; static DWORD g_vtClock, g_vtJob; static size_t g_vtBytes, g_vtBudget = (size_t)160 << 20;   /* ini VinylTintMemoryMB */
static int pic_vinyl(const char *n)
{
    return !strstr(n, "DOORLINE") && (strstr(n, "_ART_F_") || strstr(n, "_CONTEST_") || strstr(n, "_UNIQUE_") || strstr(n, "FLAMESKULL") || strstr(n, "MECHAMOTH"));
}
static DWORD *vinyl_tinted(DWORD *src, int w, int h, DWORD rgb)
{
    int i, k = -1; DWORD lut[256]; float hh, ss, vv; size_t need = (size_t)w * h * 4; static int warned;
    for (i = 0; i < 24; i++) if (g_vt[i].src == src && g_vt[i].w == w && g_vt[i].h == h && g_vt[i].rgb == rgb && g_vt[i].dst) { g_vt[i].used = ++g_vtClock; return g_vt[i].dst; }
    if (need > g_vtBudget) { if (warned++ < 3) clog("vinyl colour: %dx%d picture is bigger than VinylTintMemoryMB - left untinted", w, h); return NULL; }
    for (i = 0; i < 24 && k < 0; i++) if (!g_vt[i].dst) k = i;                       /* a free entry, else the oldest one */
    if (k < 0) for (i = 0; i < 24; i++) if (g_vt[i].used <= g_vtJob && (k < 0 || g_vt[i].used < g_vt[k].used)) k = i;   /* (not one this paint uses) */
    if (k < 0) return NULL;
    if (g_vt[k].dst && (g_vt[k].w != w || g_vt[k].h != h)) { free(g_vt[k].dst); g_vtBytes -= (size_t)g_vt[k].w * g_vt[k].h * 4; g_vt[k].dst = NULL; }
    while (!g_vt[k].dst && g_vtBytes + need > g_vtBudget) {                           /* over the budget: drop old copies first */
        int v = -1;
        for (i = 0; i < 24; i++) if (i != k && g_vt[i].dst && g_vt[i].used <= g_vtJob && (v < 0 || g_vt[i].used < g_vt[v].used)) v = i;
        if (v < 0) { if (warned++ < 3) clog("vinyl colour: VinylTintMemoryMB full - a %dx%d picture left untinted", w, h); return NULL; }
        free(g_vt[v].dst); g_vtBytes -= (size_t)g_vt[v].w * g_vt[v].h * 4; g_vt[v].dst = NULL; g_vt[v].src = NULL;
    }
    if (!g_vt[k].dst) { if (!(g_vt[k].dst = (DWORD *)malloc(need))) return NULL; g_vtBytes += need; }
    rgb_hsv(rgb, &hh, &ss, &vv);
    (void)hh; (void)ss; (void)vv;
    for (i = 0; i < 256; i++) {                 /* duotone: black -> the colour (at 75 % brightness) -> white */
        float L = i / 255.f, c3[3], o3[3]; int q;
        c3[0] = (float)((rgb >> 16) & 255); c3[1] = (float)((rgb >> 8) & 255); c3[2] = (float)(rgb & 255);
        for (q = 0; q < 3; q++) o3[q] = L <= 0.75f ? c3[q] * (L / 0.75f) : c3[q] + (255.f - c3[q]) * ((L - 0.75f) / 0.25f);
        lut[i] = ((DWORD)(min(255.f, max(0.f, o3[0])) + .5f) << 16) | ((DWORD)(min(255.f, max(0.f, o3[1])) + .5f) << 8) | (DWORD)(min(255.f, max(0.f, o3[2])) + .5f);
    }
    for (i = 0; i < w * h; i++) {                 /* integer luma (4096 x 4096 HD pictures: 16 M pixels) */
        DWORD c = src[i];
        g_vt[k].dst[i] = (c & 0xFF000000) | lut[((c >> 16 & 255) * 77 + (c >> 8 & 255) * 150 + (c & 255) * 29) >> 8];
    }
    g_vt[k].src = src; g_vt[k].w = w; g_vt[k].h = h; g_vt[k].rgb = rgb; g_vt[k].used = ++g_vtClock;
    return g_vt[k].dst;
}
static int g_vpicEdit;             /* the vinyl the shop shows now (last layer that changed) is a picture vinyl */
/* E5: the game skips a paint when the job equals the last one painted (0x611970 compares 0x89f938 with the job: layer
   hash, texture, colours, tint flag - NOT the pixel pointer). A new picture colour changes only the pixels, so the paint
   was skipped until something else changed (a click). The tint colours of each job are signed; a differing signature
   makes the compare answer "different" so the game paints. */
static DWORD g_tintSig, g_tintSigDone;
/* vinyl part -> picture or not, learned from every paint of our car (the job's vinyl layers in order = the car's vinyl
   layers 0..3 that hold a part; door lines are not vinyl parts). Lets the vinyl list know at once whether the row it
   opens on is a picture vinyl, before anything changes. */
typedef struct { DWORD part; int pic; } VPic;
static VPic g_vpm[96]; static int g_nvpm;
static void vpic_learn(DWORD part, int pic)
{
    int i; for (i = 0; i < g_nvpm && g_vpm[i].part != part; i++);
    if (i == g_nvpm) { if (g_nvpm == 96) return; g_nvpm++; }
    g_vpm[i].part = part; g_vpm[i].pic = pic;
}
static int vpic_known(DWORD part) { int i; for (i = 0; i < g_nvpm; i++) if (g_vpm[i].part == part) return g_vpm[i].pic; return -1; }
static void vinyl_tint_job(BYTE *job)
{
    const char *car; Entry *e; DWORD rgb = 0; int n, i, L, have = 0; BYTE *skin;
    if (!g_vTint || !g_enabled || !g_compMine || !(car = car_name(g_compMine)) || !readable(job, 0x1c8)) return;
    skin = *(BYTE **)job;
    if (!readable(skin, 0x50) || *(BYTE *)(skin + 0x4a) == 8) return;          /* 8-bit path (0x612280): left alone */
    n = *(int *)(job + 0x1c4); if (n < 0 || n > 8) return;
    {   /* learn which vinyl parts are pictures */
        int L2 = 0; char nm3[48];
        for (i = 0; i < n; i++) {
            BYTE *Lr = job + 0x24 + i * 0x34, *ti2 = *(BYTE **)(Lr + 4); BYTE *vp = NULL;
            if (!*(DWORD *)Lr || !readable(ti2, 0x48)) continue;
            memcpy(nm3, ti2 + 0xc, 47); nm3[47] = 0;
            if (strstr(nm3, "DOORLINE")) continue;
            while (L2 < 4 && !(vp = part_of(g_compMine, 0x40 + L2))) L2++;
            if (L2 >= 4) break;
            vpic_learn(part_hash(vp), pic_vinyl(nm3) != 0); L2++;
        }
    }
    if (*(int *)FLOW == 3) {           /* shop: the layer whose picture changed since the last paint = the vinyl being browsed */
        static DWORD prev[8]; static int pn = -1; char nm2[48];
        for (i = 0; i < n; i++) {
            BYTE *Lr = job + 0x24 + i * 0x34, *ti2 = *(BYTE **)(Lr + 4);
            if (pn >= 0 && (i >= pn || prev[i] != *(DWORD *)Lr) && readable(ti2, 0x48) && !strstr((char *)ti2 + 0xc, "DOORLINE")) {
                int was = g_vpicEdit; memcpy(nm2, ti2 + 0xc, 47); nm2[47] = 0;
                g_vpicEdit = pic_vinyl(nm2) != 0;
                if (was != g_vpicEdit) clog("vinyl shop: %s is %s", nm2, g_vpicEdit ? "a picture vinyl - Custom color button" : "a normal vinyl");
            }
            prev[i] = *(DWORD *)Lr;
        }
        pn = n;
    }
    (void)e; (void)have; (void)L;
    g_vtJob = g_vtClock;                       /* copies used from here on belong to this paint and are never dropped by it */
    g_tintSig = 0;
    {   int L3 = 0;
    for (i = 0; i < n; i++) {
        BYTE *L = job + 0x24 + i * 0x34, *ti = *(BYTE **)(L + 4); DWORD *px = *(DWORD **)(L + 0xc), *t; int w, h, lay = -1; char nm[48];
        if (!*(DWORD *)L || !readable(ti, 0x48)) continue;
        memcpy(nm, ti + 0xc, 47); nm[47] = 0;
        if (!strstr(nm, "DOORLINE")) { while (L3 < 4 && !part_of(g_compMine, 0x40 + L3)) L3++; if (L3 < 4) lay = L3++; }   /* job layer -> car layer */
        if (!px) continue;
        w = *(short *)(ti + 0x44); h = *(short *)(ti + 0x46);
        { static DWORD seen[32]; static int ns; int k2; for (k2 = 0; k2 < ns && seen[k2] != *(DWORD *)L; k2++);
          if (k2 == ns && ns < 32) { seen[ns++] = *(DWORD *)L; clog("vinyl layer %d: %s %dx%d tint flag %lu -> %s", i, nm, w, h, *(DWORD *)(L + 0x20), pic_vinyl(nm) ? "picture, gets the vinyl colour" : "left to the game"); } }
        if (!pic_vinyl(nm) || lay < 0 || w <= 0 || h <= 0 || w > 4096 || h > 4096 || !readable(px, (size_t)w * h * 4)) continue;
        if (!custom_for(g_compMine, 0x240 + lay, &rgb)) continue;          /* no colour of its own: the picture as it is */
        rgb = sat_rgb(rgb, sat_amt(g_compMine, 0x240 + lay));             /* E13 */
        if ((t = vinyl_tinted(px, w, h, rgb))) { *(DWORD **)(L + 0xc) = t; g_tintSig = g_tintSig * 31 + (rgb | 0x1000000) + (DWORD)i * 0x9E3779B1u; }
    }
    }
}
typedef int (__cdecl *PFN_VCache)(void *cache, BYTE *job);
static int __cdecl h_vcache(void *cache, BYTE *job)
{
    int same;
    g_tintSig = 0;
    __try { vinyl_tint_job(job); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) clog("vinyl tint: exception 0x%08lx", GetExceptionCode()); }
    /* E7: six paint targets, each with its own cache (0x89f050 + i * 0x1c8, i = 0..5): the tint signature is kept per cache
       (one shared one made every other decal preview skip a target last painted untinted -> the picture flipped to Default) */
    {   static DWORD sigDone[7]; DWORD ci = ((DWORD)cache - 0x89f050) / 0x1c8;
        if ((DWORD)cache < 0x89f050 || ci > 5 || ((DWORD)cache - 0x89f050) % 0x1c8) ci = 6;
        same = ((PFN_VCache)A_VCACHE)(cache, job) & 0xFF;
        if (same && g_tintSig != sigDone[ci]) {    /* same job, other picture colour: paint it */
            static int n; if (n++ < 10) clog("vinyl colour: picture colour changed on paint target %lu (%08lx -> %08lx) - re-painted", ci, sigDone[ci], g_tintSig);
            same = 0;
        }
        if (!same) sigDone[ci] = g_tintSig;          /* the game paints this job now: its colours are what this target shows */
        g_tintSigDone = g_tintSig;
    }
    return same;
}
/* rides seen in the car renderers: they get re-composited when a colour changes */
typedef struct { BYTE *ride; DWORD seen; LONG ver; DWORD done; } Seen;
static int ride_recent(BYTE *ride);
static Seen g_seen[8];
static void note_ride(BYTE *ride)
{
    int i, free_ = -1; DWORD now = GetTickCount();
    for (i = 0; i < 8; i++) {
        if (g_seen[i].ride == ride) { g_seen[i].seen = now; return; }
        if (free_ < 0 && (!g_seen[i].ride || now - g_seen[i].seen > 3000)) free_ = i;
    }
    if (free_ >= 0) { g_seen[free_].ride = ride; g_seen[free_].seen = now; g_seen[free_].ver = -1; g_seen[free_].done = 0; }
}
static DWORD g_lastComp;
static int has_paint(BYTE *ride)
{
    const char *car = car_name(ride); int i, k, n = 0;
    if (!car) return 0;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_ne; i++) if (!_stricmp(g_e[i].car, car)) {
        if (IS_VKEY(g_e[i].slot)) n++;
        for (k = 0; k < NSLOT; k++) if (SLOTS[k].slot == g_e[i].slot && (SLOTS[k].kind == K_PAINT || SLOTS[k].kind == K_SHINE)) n++;
    }
    LeaveCriticalSection(&g_cs);
    return n > 0;
}
static void recomposite_due(void)
{
    int i; DWORD now = GetTickCount(); LONG v = g_ver;
    static int s_flow; static LONG s_verWorld = -2; int fl = *(int *)FLOW;
    /* entering free roam: the game has just painted the world car with our colours (and its vinyl). Colours changed in
       the shop before that are already on it - re-painting now comes after the vinyl images are unloaded (vinyl lost). */
    if (fl == 6 && s_flow != 6) s_verWorld = v;
    s_flow = fl;
    if (!o_comp || now - g_lastComp < 120) return;
    {   /* E3: order of turns - drawn this frame first, then the one painted longest ago */
        int best = -1, bl = 0; static int s_ord[8]; int k, n = 0;
        for (i = 0; i < 8; i++) {
            BYTE *r = g_seen[i].ride; int l;
            if (!r || now - g_seen[i].seen > 3000 || g_seen[i].ver == v) continue;
            l = ride_recent(r);
            if (best < 0 || l > bl || (l == bl && g_seen[i].done < g_seen[best].done)) { best = i; bl = l; }
        }
        if (best >= 0) s_ord[n++] = best;
        for (k = 0; k < 8; k++) if (k != best) s_ord[n++] = k;
        for (k = 0; k < n; k++) {
        BYTE *r; i = s_ord[k]; r = g_seen[i].ride;
        if (!r || now - g_seen[i].seen > 3000 || g_seen[i].ver == v) continue;
        if (g_seen[i].ver == -1 && !has_paint(r)) { g_seen[i].ver = v; continue; }   /* first sighting, nothing to put on */
        /* first sighting of the free-roam car: the game already painted it at load with our colours (player copies are
           recognised since 16.11 19:15). Painting it again here came after the vinyl images were unloaded -> the car lost
           its vinyl at game start until a shop visit. Only colour changes made later re-paint it. */
        if (r == (BYTE *)WORLD_RIDE && fl == 6 && (g_seen[i].ver == -1 || v == s_verWorld)) { g_seen[i].ver = v; clog("world car: painted by the game at load - no extra re-paint"); continue; }
        g_seen[i].ver = v;
        if (!mine(r) || !*(DWORD *)(r + 0x584)) continue;      /* not ours / no composited skin */
        __try { h_comp(r); } __except (EXCEPTION_EXECUTE_HANDLER) { clog("re-composite of %p crashed", r); }
        g_lastComp = now; g_seen[i].done = now;
        return;                                                 /* one per call: keeps frames smooth */
        }
    }
}

/* neon + headlight: written into the car's render info right before its neon is drawn */
typedef void (__fastcall *PFN_NeonCol)(BYTE *cri, void *edx, BYTE *part, BYTE *base, BYTE *delta);
static const PFN_NeonCol NeonCol = (PFN_NeonCol)A_NEONCOL;
typedef struct { BYTE *cri; BYTE mask; BYTE *orig; DWORD key; BYTE mat[0xa8]; BYTE *psrc[8]; DWORD pkey[8]; BYTE pm[8][0xa8]; } CriState;   /* mat = body copy, pm = part copies */   /* mat = per-car paint material copy */
static CriState g_cri[16];
static CriState *cri_state(BYTE *cri)
{
    int i, f = -1;
    for (i = 0; i < 16; i++) { if (g_cri[i].cri == cri) return &g_cri[i]; if (f < 0 && !g_cri[i].mask) f = i; }
    if (f < 0) f = (int)(GetTickCount() & 15);
    g_cri[f].cri = cri; g_cri[f].mask = 0;
    return &g_cri[f];
}
/* Body shine. The car shader (vs_1_1 in the exe) colours the paint with
     skin * (DiffuseMin + DiffuseRange * max(dot(view, normal), 0.01))      DiffuseRange = Max*MaxScale - Min*MinScale
   so the light material's "Max" colour is head-on and "Min" is the edge / glancing colour (stock pearls: PEARL1 blue edges,
   red face). Specular min/range and envmap min/range work the same way. cri+0x384 = the body paint's light material
   (0xa8 bytes: +0x30 MinScale, +0x34 Min RGB, +0x40 MaxScale, +0x44 Max RGB, +0x5c/+0x60 spec min, +0x6c/+0x70 spec max,
   +0x80/+0x84 env min, +0x90/+0x94 env max). The plugin points the car at its own copy with a neutral face and the shine
   colour at the edges; the shared original is never written. */
static DWORD game_rgb(BYTE *ride, int slot);
static float g_shineMax = 1.6f;      /* ini ShineStrength: highest edge multiplier (more = stronger shine, but trim gets tinted) */
static float g_partBright = 1.35f;            /* ini PartBrightness: least diffuse multiplier for custom-coloured parts */
static float g_rimBright = 2.0f;              /* ini RimBrightness: the same for rims / spinners (their texture is darker: 16.11 rims looked dull) */
static float avg3(const float *v) { return (v[0] + v[1] + v[2]) / 3.f; }
static DWORD mix_rgb(DWORD a, DWORD b, float t);
static void make_shine(BYTE *dst, const BYTE *src, DWORD base, DWORD shine, float kfloor, float amt, float sat)
{
    /* skin = base colour (channels lifted to >= 0x28, see shine_skin). shader colour = skin * (Min + (Max - Min) * facing):
       Max (head-on) stays NEUTRAL, so the paint shows its base colour and lights / lenses / badges on the body skin keep
       their own colour; Min (edges) = shine / skin per channel, so the edges turn into the shine colour */
    float *f = (float *)dst; const float *o = (const float *)src; float k, sn[3], mx = 0, a, t = amt > 1.f ? amt - 1.f : 0.f; int c;
    DWORD sk0 = shine_skin2(base, sat), sk = t > 0 ? shine_skin2(mix_rgb(base, shine, 0.85f * t), sat) : sk0, face = mix_rgb(sk0, shine, 0.08f * t);
    memcpy(dst, src, 0xa8);
    k = avg3(o + 0x44 / 4) * o[0x40 / 4];
    if (k < 0.8f) k = 0.8f;
    if (k > 1.6f) k = 1.6f;
    if (k < kfloor) k = kfloor;
    f[0x40 / 4] = 1.f; f[0x30 / 4] = 1.f;
    for (c = 0; c < 3; c++) {
        float kc = ((sk >> (16 - 8 * c)) & 255) / 255.f, sc = ((shine >> (16 - 8 * c)) & 255) / 255.f, m = sc / kc * k,
              fc = ((face >> (16 - 8 * c)) & 255) / 255.f / kc * k, cap = g_shineMax * (1.f + t);
        f[0x44 / 4 + c] = t > 0 ? (fc > 2.f ? 2.f : fc) : k;       /* face: neutral (> 100 %: base look over the shine-mixed skin) */
        f[0x34 / 4 + c] = m > cap ? cap : m;                 /* edges: shine colour; capped so dark trim / rubber on the skin stays dark */
        sn[c] = sc; if (sc > mx) mx = sc;
    }
    for (c = 0; c < 3; c++) sn[c] = mx < 0.02f ? 0.2f : sn[c] / mx;  /* shine hue for edge highlights / reflections */
    /* highlights at the edges: half-tinted with the shine hue. Reflections (envmap) stay neutral: in races the sky
       reflection is bright and a tinted envmap coloured the window trim / rubber, which reflect too */
    a = avg3(o + 0x60 / 4); for (c = 0; c < 3; c++) f[0x60 / 4 + c] = a * (0.5f + 0.5f * sn[c]);
    a = avg3(o + 0x70 / 4); for (c = 0; c < 3; c++) f[0x70 / 4 + c] = a;
    a = avg3(o + 0x84 / 4); for (c = 0; c < 3; c++) f[0x84 / 4 + c] = a;
    a = avg3(o + 0x94 / 4); for (c = 0; c < 3; c++) f[0x94 / 4 + c] = a;
}
/* The car renderers read the body paint material from cri+0x384 (and +0x39c, a copy of it) right before binding it to the
   car's solids (0x48d860). Each of those reads (mov reg, [reg+0x384/0x39c]) jumps to a small stub that asks paint_mat()
   which material to use: the car's own copy when it has a custom shine, else the game's value. cri+0x384 is never written. */
/* neutral copy of a light material: same brightness / gloss / reflection strength, no colour tint, so a custom colour shows as is */
static void make_neutral(BYTE *dst, const BYTE *src, float floorFace)
{
    float *f = (float *)dst; const float *o = (const float *)src; float a; int c;
    static const int sc[4] = { 0x30, 0x40, 0x5c, 0x6c }, rgb[4] = { 0x34, 0x44, 0x60, 0x70 };
    int k;
    memcpy(dst, src, 0xa8);
    for (k = 0; k < 4; k++) {
        a = avg3(o + rgb[k] / 4) * o[sc[k] / 4];
        if (k == 1 && a < floorFace) a = floorFace;                     /* head-on diffuse: bright enough to match the swatch */
        if (k == 0 && a < floorFace * 0.85f) a = floorFace * 0.85f;     /* edges a little darker, like the game's paints */
        f[sc[k] / 4] = 1.f; for (c = 0; c < 3; c++) f[rgb[k] / 4 + c] = a;
    }
    a = avg3(o + 0x84 / 4); for (c = 0; c < 3; c++) f[0x84 / 4 + c] = a;
    a = avg3(o + 0x94 / 4); for (c = 0; c < 3; c++) f[0x94 / 4 + c] = a;
}
/* render-info material field -> paint slot (from the setup 0x638190: +0x390 rim, +0x394 exhaust, +0x398 calipers,
   +0x39c trunk audio (or the body material when no audio), +0x3a0 spoiler, +0x3a4 roof scoop, +0x3a8 mirrors) */
static int mat_slot(DWORD off)
{
    switch (off) { case 0x390: return 0x49; case 0x394: return 0x47; case 0x398: return 0x46; case 0x39c: return 0x48;
                   case 0x3a0: return 0x45; case 0x3a4: return 0x4b; case 0x3a8: return 0x4c; }
    return -1;
}
/* Composited textures (body skin, rims, ...) are linked to the car's models through 3 texture tables in the render info,
   one per detail level: +0x608.. (near), +0xa98.. (+0x490) and +0xf28.. (+0x920); entries of 16 bytes
   {placeholder hash, new hash, TextureInfo*, ?}. Only the near table gets the TextureInfo of the composited texture; the
   others keep -1, so a farther (smaller) model - the wheels while driving - shows the stock texture. Fill them in. */
static int g_rimLodFix;      /* ini RimLodFix (default 0): suspected of the 18:30 crash on unloading the drive (model texture lists) */
static void fix_lod_textures(BYTE *cri)
{
    int a, k;
    if (!readable(cri + 0x608, 0x920 + 0x490)) return;
    for (k = 1; k <= 2; k++)
        for (a = 0x608; a < 0x608 + 0x490; a += 16) {
            DWORD *ea = (DWORD *)(cri + a), *eb = (DWORD *)(cri + a + k * 0x490);
            if (!ea[0] || !ea[1] || ea[2] == 0xFFFFFFFF || !ea[2]) continue;
            if (eb[0] == ea[0] && eb[1] == ea[1] && eb[2] == 0xFFFFFFFF && readable((void *)ea[2], 0x30)) eb[2] = ea[2];
        }
}
/* ---------------- decal colours (D1, 17.11) ----------------
   CarRenderInfo +0x13e8: 7 decal places (hood, windshield, rear window, left / right door, left / right quarter) x 8 entries
   of 16 bytes {placeholder DUMMY_DECAL1..8 (hash 0x910e6654 + j), decal texture hash (NAME + _WIDE/_RECT/_SQUARE),
   TextureInfo*, ?} (seen live 17.11 02:3x: APEXI_WHITE_WIDE / STREETGLOW_WIDE on the windows; the game resolves the
   TextureInfo there). For our car each decal of a place with a custom colour points at a tinted copy: TextureInfo (0x7c)
   + its PlatInfo (TI+0, D3D texture at +0x18) cloned, with our own MANAGED texture; white -> the colour, black stays
   black (colour endpoints of the DXT blocks scaled by their brightness). The game's own textures are never written.
   Copies are cached per (texture, colour) and never freed (a table may still point at one). */
static int g_decalOn = 1, g_decalPlace = -1, g_decalTex = -1;   /* g_decalTex: the decal slot (0x61..0x98) the shop last showed a decal in */ static BYTE *g_decalScr; static void *g_dmTramp;
static int decal_screen_place(void)           /* the decal place shown on the decal screen, -1 = unknown */
{
    int sl;
    if (!g_decalScr || !readable(g_decalScr, 0x60) || *(DWORD *)g_decalScr != 0x79ca94) return -1;
    sl = *(int *)(g_decalScr + 0x4c);
    return sl >= 0x34 && sl <= 0x3a ? sl - 0x34 : sl >= 0x61 && sl <= 0x98 ? (sl - 0x61) / 8 : -1;
}
static DWORD g_decalToast; static int g_shopMode, g_dirty; static BYTE *g_decalLblPart; static char g_decalLblPk[64];   /* (defined again further down: a tentative definition) */
static int __cdecl decal_msg(BYTE *scr, DWORD msg)   /* Install with our picker open on the decal that is already fitted = colour only */
{
    BYTE *cur, *part; int sl;
    if (msg != 0x406415e3 || !g_shopMode || !g_decalOn || !readable(scr, 0x60)) return 0;
    cur = *(BYTE **)(scr + 0x5c); sl = *(int *)(scr + 0x4c);
    if (!readable(cur, 0x30) || sl < 0 || sl > 0xa9 || !(part = *(BYTE **)(cur + 0x2c)) || part != part_of((BYTE *)FE_RIDE, sl)) return 0;
    g_decalToast = GetTickCount(); g_decalLblPart = part; strncpy(g_decalLblPk, *(char **)(scr + 4) && readable(*(char **)(scr + 4), 8) ? *(char **)(scr + 4) : "", 63);
    __try { ((void (__cdecl *)(const char *, DWORD, const char *))0x537b80)(*(char **)(scr + 4), 0xd28b9316, "COLOUR INSTALLED"); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_dirty) { g_dirty = 0; EnterCriticalSection(&g_cs); save_store(); LeaveCriticalSection(&g_cs); }
    clog("decal shop: Install on the fitted decal - colour kept (the game's install skipped)");
    return 1;
}
__declspec(naked) static void h_decalmsg(void)
{
    __asm {
        mov g_decalScr, ecx
        push ecx
        push dword ptr [esp + 8]
        push ecx
        call decal_msg
        add esp, 8
        pop ecx
        test eax, eax
        jz orig
        ret 0x10
    orig:
        jmp dword ptr [g_dmTramp]
    }
}
static WORD tint565(WORD c, DWORD rgb, float inv)
{
    float r = ((c >> 11) & 31) / 31.f, g = ((c >> 5) & 63) / 63.f, b = (c & 31) / 31.f, l = (0.299f * r + 0.587f * g + 0.114f * b) * inv;
    int R, G, B;
    if (l > 1.f) l = 1.f;
    R = (int)(((rgb >> 16) & 255) * l / 255.f * 31.f + .5f); G = (int)(((rgb >> 8) & 255) * l / 255.f * 63.f + .5f); B = (int)((rgb & 255) * l / 255.f * 31.f + .5f);
    return (WORD)((R << 11) | (G << 5) | B);
}
static void tint_block(BYTE *cb, DWORD rgb, float inv, int dxt1)   /* cb = DXT colour block: c0, c1 (565), 16 x 2-bit indices */
{
    WORD c0 = *(WORD *)cb, c1 = *(WORD *)(cb + 2), n0 = tint565(c0, rgb, inv), n1 = tint565(c1, rgb, inv); DWORD idx = *(DWORD *)(cb + 4); int i;
    int four = !dxt1 || c0 > c1;              /* DXT1: c0 > c1 = 4 colours, else 3 colours + transparent */
    if (four) {
        if (n0 == n1) { *(WORD *)cb = n0; *(WORD *)(cb + 2) = n1; if (dxt1) { if (n0 < 0xFFFF) n0++; else n1--; *(WORD *)cb = n0; *(WORD *)(cb + 2) = n1; } *(DWORD *)(cb + 4) = 0; return; }
        if (dxt1 && n0 < n1) { WORD t = n0; n0 = n1; n1 = t; idx ^= 0x55555555; }      /* keep the 4-colour order: swap 0<->1, 2<->3 */
    } else if (n0 > n1) {                                                                /* keep the 3-colour order: swap 0<->1 */
        WORD t = n0; n0 = n1; n1 = t;
        for (i = 0; i < 16; i++) { DWORD v = (idx >> (2 * i)) & 3; if (v < 2) idx ^= 1u << (2 * i); }
    }
    *(WORD *)cb = n0; *(WORD *)(cb + 2) = n1; *(DWORD *)(cb + 4) = idx;
}
/* one tinted copy per decal texture; a new colour re-tints that copy's texture in place (dragging in the picker made a new
   copy for every colour before - 64 of them and the cache was full) */
static int decal_retint(IDirect3DTexture9 *tex, IDirect3DTexture9 *nt, DWORD rgb, int dxt)
{
    DWORD lv = IDirect3DTexture9_GetLevelCount(tex), L; int bs = dxt == 1 ? 8 : 16, co = dxt == 1 ? 0 : 8; float mx = 0.f, inv = 0.f;
    for (L = 0; L < lv; L++) {
        D3DLOCKED_RECT a, b; D3DSURFACE_DESC d2; int rows, y, x, cols;
        IDirect3DTexture9_GetLevelDesc(tex, L, &d2);
        if (FAILED(IDirect3DTexture9_LockRect(tex, L, &a, NULL, D3DLOCK_READONLY))) return 0;
        if (FAILED(IDirect3DTexture9_LockRect(nt, L, &b, NULL, 0))) { IDirect3DTexture9_UnlockRect(tex, L); return 0; }
        rows = dxt ? ((int)d2.Height + 3) / 4 : (int)d2.Height; cols = dxt ? ((int)d2.Width + 3) / 4 : (int)d2.Width;
        for (y = 0; y < rows; y++) memcpy((BYTE *)b.pBits + y * b.Pitch, (BYTE *)a.pBits + y * a.Pitch, dxt ? cols * bs : cols * 4);
        IDirect3DTexture9_UnlockRect(tex, L);
        if (L == 0) {                                  /* brightest colour of the picture -> becomes the custom colour */
            for (y = 0; y < rows; y++) for (x = 0; x < cols; x++) {
                if (dxt) { BYTE *cb = (BYTE *)b.pBits + y * b.Pitch + x * bs + co; int k2; for (k2 = 0; k2 < 2; k2++) { WORD c = *(WORD *)(cb + 2 * k2);
                    float l = 0.299f * ((c >> 11) & 31) / 31.f + 0.587f * ((c >> 5) & 63) / 63.f + 0.114f * (c & 31) / 31.f; if (l > mx) mx = l; } }
                else { DWORD c = *(DWORD *)((BYTE *)b.pBits + y * b.Pitch + x * 4); float l = (0.299f * ((c >> 16) & 255) + 0.587f * ((c >> 8) & 255) + 0.114f * (c & 255)) / 255.f; if (l > mx) mx = l; }
            }
            inv = mx > 0.08f ? 1.f / mx : 0.f;         /* an all-black decal: flat colour */
        }
        for (y = 0; y < rows; y++) for (x = 0; x < cols; x++) {
            if (dxt) {
                BYTE *cb = (BYTE *)b.pBits + y * b.Pitch + x * bs + co;
                if (inv == 0.f) { *(WORD *)cb = *(WORD *)(cb + 2) = (WORD)((((rgb >> 19) & 31) << 11) | (((rgb >> 10) & 63) << 5) | ((rgb >> 3) & 31)); if (dxt != 1) *(DWORD *)(cb + 4) = 0; }
                else tint_block(cb, rgb, inv, dxt == 1);
            } else {
                DWORD *px = (DWORD *)((BYTE *)b.pBits + y * b.Pitch + x * 4), c = *px;
                float l = inv == 0.f ? 1.f : (0.299f * ((c >> 16) & 255) + 0.587f * ((c >> 8) & 255) + 0.114f * (c & 255)) / 255.f * inv;
                if (l > 1.f) l = 1.f;
                *px = (c & 0xFF000000) | ((DWORD)(((rgb >> 16) & 255) * l) << 16) | ((DWORD)(((rgb >> 8) & 255) * l) << 8) | (DWORD)((rgb & 255) * l);
            }
        }
        IDirect3DTexture9_UnlockRect(nt, L);
    }
    return 1;
}
/* HD decal packs (2048 / 4096, DXT1-5 or 32-bit): each copy's texture memory is counted (ini DecalTintMemoryMB). Over the
   budget, copies no car has drawn for 3 s give their texture back; the small copy itself stays (its texture pointer goes
   back to the untinted picture, so a car that still holds it never sees freed memory) and it is tinted again when used. */
typedef struct { BYTE *orig, *clone, *ride; DWORD rgb, t, seen, hash; int dxt, sl; size_t bytes; IDirect3DTexture9 *nt; } DClone2;
static BYTE *g_decalRide;          /* the car decal_fix works on (stored with each copy for the per-frame re-tint) */
#define NDC2 512
static DClone2 g_dc2[NDC2]; static int g_ndc2; static size_t g_dcBytes, g_dcBudget = (size_t)192 << 20;
static BYTE *decal_white(BYTE *ti);
static size_t tex_bytes(D3DSURFACE_DESC *d0, DWORD lv, int dxt)
{
    size_t b = 0; DWORD L, w = d0->Width, h = d0->Height;
    for (L = 0; L < lv; L++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        b += dxt ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (dxt == 1 ? 8 : 16) : (size_t)w * h * 4;
    return b;
}
static void dc_release(DClone2 *e, IDirect3DTexture9 *orig)
{
    *(IDirect3DTexture9 **)(e->clone + 0x7c + 0x18) = orig;
    IDirect3DTexture9_Release(e->nt); e->nt = NULL; g_dcBytes -= e->bytes; e->rgb = 0xFFFFFFFF;
}
static int dc_room(size_t need, DClone2 *keep)          /* make room for need bytes; 0 = the budget is full of copies in use */
{
    DWORD now = GetTickCount();
    while (g_dcBytes + need > g_dcBudget) {
        int i, v = -1;
        for (i = 0; i < g_ndc2; i++) { DClone2 *e = &g_dc2[i];
            if (e != keep && e->nt && now - e->seen > 3000 && (v < 0 || e->seen < g_dc2[v].seen)) v = i; }
        if (v < 0) return 0;
        { BYTE *pi = *(BYTE **)decal_white(g_dc2[v].orig); dc_release(&g_dc2[v], readable(pi, 0x20) ? *(IDirect3DTexture9 **)(pi + 0x18) : NULL); }
    }
    return 1;
}
static int dc_make(DClone2 *e, IDirect3DTexture9 *tex, DWORD rgb)   /* (re)create the copy's own texture, tinted */
{
    D3DSURFACE_DESC dsc; DWORD lv = IDirect3DTexture9_GetLevelCount(tex); IDirect3DDevice9 *dev = NULL; IDirect3DTexture9 *nt = NULL; size_t need; static int warned;
    if (FAILED(IDirect3DTexture9_GetLevelDesc(tex, 0, &dsc))) return 0;
    need = tex_bytes(&dsc, lv, e->dxt);
    if (!dc_room(need, e)) { if (warned++ < 5) clog("decal colour: DecalTintMemoryMB full - %s left untinted", (char *)e->orig + 0xc); return 0; }
    IDirect3DTexture9_GetDevice(tex, &dev);
    if (!dev || FAILED(IDirect3DDevice9_CreateTexture(dev, dsc.Width, dsc.Height, lv, 0, dsc.Format, D3DPOOL_MANAGED, &nt, NULL))) {
        if (dev) IDirect3DDevice9_Release(dev); clog("decal colour: no texture for %s", (char *)e->orig + 0xc); return 0; }
    IDirect3DDevice9_Release(dev);
    if (!decal_retint(tex, nt, rgb, e->dxt)) { IDirect3DTexture9_Release(nt); clog("decal colour: cannot read %s", (char *)e->orig + 0xc); return 0; }
    e->nt = nt; e->bytes = need; g_dcBytes += need; e->rgb = rgb; e->t = GetTickCount();
    if (e->clone) *(IDirect3DTexture9 **)(e->clone + 0x7c + 0x18) = nt;
    return 1;
}
static BYTE *decal_clone2(BYTE *ti, BYTE *src, DWORD rgb, int sl);
static BYTE *decal_white(BYTE *ti)            /* the white version of a decal picture (itself if it is white / not found) */
{
    char nm[64], *p; size_t n; DWORD hv = 0xFFFFFFFF; BYTE *w; const char *q;
    if (!readable(ti, 0x30)) return ti;
    memcpy(nm, ti + 0xc, 0x18); nm[0x18] = 0;
    if (strstr(nm, "_WHITE_") || !(p = strrchr(nm, '_'))) return ti;
    n = strlen(p); if ((p - nm) + 6 + n >= sizeof nm) return ti;
    memmove(p + 6, p, n + 1); memcpy(p, "_WHITE", 6);
    for (q = nm; *q; q++) hv = hv * 33 + (BYTE)*q;
    __try { w = ((BYTE *(__cdecl *)(DWORD, int, int))0x4901d0)(hv, 0, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { w = NULL; }
    return w && readable(w, 0x7c) && *(DWORD *)(w + 0x24) == hv ? w : ti;
}
static BYTE *decal_clone(BYTE *ti, DWORD rgb, int sl) { return decal_clone2(ti, decal_white(ti), rgb, sl); }
static BYTE *decal_clone2(BYTE *ti, BYTE *src, DWORD rgb, int sl)   /* one copy per (decal picture, decal slot) */
{
    BYTE *pi, *c; IDirect3DTexture9 *tex; D3DSURFACE_DESC dsc; int i, dxt; DClone2 *e;
    pi = *(BYTE **)src;
    if (!readable(pi, 0x20) || !(tex = *(IDirect3DTexture9 **)(pi + 0x18))) return NULL;
    for (i = 0; i < g_ndc2; i++) if (g_dc2[i].orig == ti && g_dc2[i].sl == sl) {
        e = &g_dc2[i];
        if (e->clone && e->hash != *(DWORD *)(ti + 0x24)) {
            /* E10: the decal list streams its pictures through a few reused texture slots - this slot holds ANOTHER picture
               now; our copy still showed the old one (3-4 pictures repeated while scrolling, right again after a recolour) */
            BYTE *c = e->clone; int nd;
            if (FAILED(IDirect3DTexture9_GetLevelDesc(tex, 0, &dsc))) return NULL;
            nd = dsc.Format == D3DFMT_DXT1 ? 1 : (dsc.Format == D3DFMT_DXT2 || dsc.Format == D3DFMT_DXT3 || dsc.Format == D3DFMT_DXT4 || dsc.Format == D3DFMT_DXT5) ? 2
               : (dsc.Format == D3DFMT_A8R8G8B8 || dsc.Format == D3DFMT_X8R8G8B8) ? 0 : -1;
            *(IDirect3DTexture9 **)(c + 0x7c + 0x18) = tex;          /* (never a freed texture in between) */
            if (e->nt) { IDirect3DTexture9_Release(e->nt); g_dcBytes -= e->bytes; e->nt = NULL; e->bytes = 0; }
            memcpy(c, src, 0x7c); memcpy(c + 0x7c, pi, 0x60);
            *(BYTE **)c = c + 0x7c; *(BYTE **)(c + 4) = NULL; *(BYTE **)(c + 8) = NULL;
            *(BYTE **)(c + 0x7c) = c + 0x7c; *(BYTE **)(c + 0x7c + 4) = c + 0x7c;
            *(IDirect3DTexture9 **)(c + 0x7c + 0x18) = tex;
            e->hash = *(DWORD *)(ti + 0x24); e->dxt = nd; e->rgb = 0xFFFFFFFF;
            { static int n; if (n++ < 40) clog("decal colour: picture slot %p now holds %s - its colour copy is rebuilt", ti, (char *)ti + 0xc); }
            if (nd < 0) return NULL;
            return dc_make(e, tex, rgb) ? e->clone : NULL;
        }
        if (!e->clone) return NULL;                                  /* this texture cannot be tinted */
        e->seen = GetTickCount(); e->ride = g_decalRide;
        if (!e->nt) return dc_make(e, tex, rgb) ? e->clone : NULL;   /* its texture was given back: tint it again */
        if (e->rgb != rgb && GetTickCount() - e->t > 40) {           /* new colour: re-tint the same copy (at most 25 x a second) */
            if (decal_retint(tex, e->nt, rgb, e->dxt)) e->rgb = rgb;
            e->t = GetTickCount();
        }
        return e->clone;
    }
    if (g_ndc2 >= NDC2) return NULL;
    if (FAILED(IDirect3DTexture9_GetLevelDesc(tex, 0, &dsc))) return NULL;
    dxt = dsc.Format == D3DFMT_DXT1 ? 1 : (dsc.Format == D3DFMT_DXT2 || dsc.Format == D3DFMT_DXT3 || dsc.Format == D3DFMT_DXT4 || dsc.Format == D3DFMT_DXT5) ? 2
        : (dsc.Format == D3DFMT_A8R8G8B8 || dsc.Format == D3DFMT_X8R8G8B8) ? 0 : -1;
    e = &g_dc2[g_ndc2]; memset(e, 0, sizeof *e); e->orig = ti; e->sl = sl; e->dxt = dxt; e->seen = GetTickCount(); e->ride = g_decalRide; e->hash = *(DWORD *)(ti + 0x24);
    if (dxt < 0) { clog("decal colour: %s has format %08lx - not tinted", (char *)ti + 0xc, (DWORD)dsc.Format); g_ndc2++; return NULL; }
    if (!dc_make(e, tex, rgb)) { g_ndc2++; return NULL; }
    c = (BYTE *)calloc(1, 0x7c + 0x60);
    if (!c) { IDirect3DTexture9_Release(e->nt); g_dcBytes -= e->bytes; e->nt = NULL; g_ndc2++; return NULL; }
    memcpy(c, src, 0x7c); memcpy(c + 0x7c, pi, 0x60);
    *(BYTE **)c = c + 0x7c;                                   /* own PlatInfo */
    *(BYTE **)(c + 4) = NULL; *(BYTE **)(c + 8) = NULL;       /* in no texture list */
    *(BYTE **)(c + 0x7c) = c + 0x7c; *(BYTE **)(c + 0x7c + 4) = c + 0x7c;
    *(IDirect3DTexture9 **)(c + 0x7c + 0x18) = e->nt;
    e->clone = c; g_ndc2++;
    clog("decal colour: %s (from %s) %ux%u (%s, %lu levels, %u KB, %u MB in use) tinted %06lX", (char *)ti + 0xc, (char *)src + 0xc, dsc.Width, dsc.Height,
         dxt == 1 ? "DXT1" : dxt == 2 ? "DXT2-5" : "32-bit", IDirect3DTexture9_GetLevelCount(tex), (unsigned)(e->bytes >> 10), (unsigned)(g_dcBytes >> 20), rgb);
    return c;
}
/* E1: every frame, copies a car showed in the last 2 s follow their slot's stored colour at once (a hue click, a My colours
   swatch, arrow keys - not only when the game happens to re-read the car's materials) */
static int ride_live(BYTE *ride);
static void decal_sweep(void)
{
    int i; DWORD now = GetTickCount(), rgb;
    if (!g_decalOn || !g_enabled) return;
    __try {
        for (i = 0; i < g_ndc2; i++) {
            DClone2 *e = &g_dc2[i]; BYTE *src, *pi; IDirect3DTexture9 *tex;
            if (!e->clone || !e->nt || !e->ride || now - e->seen > 2000 || now - e->t < 30 || !ride_live(e->ride)) continue;
            if (!readable(e->orig, 0x28) || *(DWORD *)(e->orig + 0x24) != e->hash) continue;   /* E10: slot reused - rebuilt by decal_clone2 */
            if (!readable(e->ride, 0x590 + 170 * 4) || !custom_for(e->ride, 0x200 | (0x61 + e->sl), &rgb)) continue;
            rgb = sat_rgb(rgb, sat_amt(e->ride, 0x200 | (0x61 + e->sl)));
            if (rgb == e->rgb) continue;
            src = decal_white(e->orig); pi = readable(src, 0x7c) ? *(BYTE **)src : NULL;
            if (!readable(pi, 0x20) || !(tex = *(IDirect3DTexture9 **)(pi + 0x18))) continue;
            if (decal_retint(tex, e->nt, rgb, e->dxt)) e->rgb = rgb;
            e->t = now;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 3) clog("decal colour: sweep exception 0x%08lx", GetExceptionCode()); }
}
static BYTE *decal_orig(BYTE *ti) { int i; for (i = 0; i < g_ndc2; i++) if (g_dc2[i].clone == ti) return g_dc2[i].orig; return ti; }
static int custom_for(BYTE *ride, int slot, DWORD *rgb);
static void decal_fix(BYTE *cri, BYTE *ride)
{
    int k, j, has; DWORD rgb;
    if (!g_decalOn || !readable(cri + 0x13e8, 0x380)) return;
    g_decalRide = ride;
    for (k = 0; k < 7; k++) {
        for (j = 0; j < 8; j++) {
            has = part_of(ride, 0x61 + k * 8 + j) && custom_for(ride, 0x200 | (0x61 + k * 8 + j), &rgb);   /* each decal slot its own colour */
            if (has) rgb = sat_rgb(rgb, sat_amt(ride, 0x200 | (0x61 + k * 8 + j)));                        /* E13 */
            DWORD *e = (DWORD *)(cri + 0x13e8 + k * 0x80 + j * 16); BYTE *ti = (BYTE *)e[2], *o, *c;
            if (e[0] != 0x910e6654 + (DWORD)j || !ti || ti == (BYTE *)-1) continue;          /* DUMMY_DECAL1..8 */
            if (e[1] == 0x66994f4a) { if (decal_orig(ti) != ti) e[2] = (DWORD)decal_orig(ti); continue; }   /* DEFAULTALPHA = empty spot */
            o = decal_orig(ti);
            if (!readable(o, 0x7c) || *(DWORD *)(o + 0x24) != e[1]) {
                /* E8: the spot shows another decal now (the list moved on) but still holds our copy of the old picture:
                   the picture of the new name, else the copy's own original (APEX stayed on the car while ABS DYNAMICS was listed) */
                BYTE *w = NULL;
                __try { w = ((BYTE *(__cdecl *)(DWORD, int, int))0x4901d0)(e[1], 0, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { w = NULL; }
                if (w && readable(w, 0x7c) && *(DWORD *)(w + 0x24) == e[1]) { o = w; e[2] = (DWORD)o; }
                else if (o != ti) e[2] = (DWORD)o;
                { static int n; if (n++ < 60) clog("decal spot %d/%d (cri %p): listed %08lx, held %08lx -> %s", k, j, cri, e[1], (DWORD)ti, w && o == w ? (char *)w + 0xc : "its own picture"); }
            }
            if (!has) { if (o != ti) e[2] = (DWORD)o; continue; }
            if (!readable(o, 0x7c) || *(DWORD *)(o + 0x24) != e[1]) continue;
            if (!(c = decal_clone(o, rgb, k * 8 + j))) c = o;
            if ((BYTE *)e[2] != c) e[2] = (DWORD)c;
        }
    }
}
static DWORD shine_amount(BYTE *ride, int slot, DWORD base, DWORD shine)    /* D9: shine colour scaled towards the base */
{
    DWORD a; float f; int k; DWORD out = 0;
    if (!custom_for(ride, 0x300 | (slot & 0xFF), &a)) return shine;
    f = (a & 255) / 128.f; if (f >= 1.f) return shine;
    for (k = 16; k >= 0; k -= 8) out |= (DWORD)(((base >> k) & 255) * (1 - f) + ((shine >> k) & 255) * f + .5f) << k;
    return out;
}
static BYTE *paint_mat_(BYTE *cri, DWORD off)
{
    BYTE *v = *(BYTE **)(cri + off), *mat = *(BYTE **)(cri + 0x384), *ride = *(BYTE **)(cri + 4); CriState *st; DWORD srgb, brgb, key;
    if (!v || !readable(v, 0xa8) || !readable(ride, 0x590 + 170 * 4) || !g_kindOn[K_PAINT] || !mine(ride)) return v;
    if (off == 0x384) { __try { decal_fix(cri, ride); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) clog("decal colour: exception 0x%08lx", GetExceptionCode()); } }
    if (off == 0x384 && g_rimLodFix) fix_lod_textures(cri);
    if (off == 0x384 || (off == 0x39c && v == mat)) {                       /* body paint */
        int shine = g_kindOn[K_SHINE] && custom_for(ride, SHINE_SLOT, &srgb), base = custom_for(ride, 0x3f, &brgb);
        if (!shine && !base) return v;
        st = cri_state(cri);
        if (!base) brgb = game_rgb(ride, 0x3f);
        {   float sa = sat_amt(ride, 0x3f);
            brgb = sat_rgb(brgb, sa); if (shine) srgb = sat_rgb(srgb, sa);
            if (shine) srgb = shine_amount(ride, 0x3f, brgb, srgb);
            key = shine ? srgb ^ (brgb * 31) ^ (DWORD)v ^ ((DWORD)(shine_amt(ride, 0x3f) * 1000.f) * 2654435761u) ^ ((DWORD)(sa * 1000.f) * 40503u) : 0x5A5A5A5A ^ (DWORD)v;
        if (!(st->mask & 0x20) || st->key != key) {
            if (shine) make_shine(st->mat, v, brgb, srgb, 0.f, shine_amt(ride, 0x3f), sa); else make_neutral(st->mat, v, 0.f);
            st->key = key; st->orig = v; st->mask |= 0x20;
            clog("body: car %p uses its own copy of %s (%s)", cri, (char *)v + 0x14, shine ? "base + shine" : "neutral");
        }
        }
        return st->mat;
    } else {                                                                 /* a painted part */
        int slot = mat_slot(off), i = (off - 0x390) / 4, has, sh; DWORD rgb, srgb = 0, key;
        if (slot < 0 || i < 0 || i > 7) return v;
        has = custom_for(ride, slot, &rgb);
        sh = g_kindOn[K_SHINE] && custom_for(ride, 0x100 | slot, &srgb);
        if (!has && !sh) return v;
        if (!has) rgb = game_rgb(ride, slot);
        {   float sa = sat_amt(ride, slot);
            rgb = sat_rgb(rgb, sa); if (sh) srgb = sat_rgb(srgb, sa);
        if (sh) srgb = shine_amount(ride, slot, rgb, srgb);
        st = cri_state(cri);
        key = sh ? (srgb ^ (rgb * 31) ^ 0x80000000 ^ ((DWORD)(shine_amt(ride, slot) * 1000.f) * 2654435761u) ^ ((DWORD)(sa * 1000.f) * 40503u)) : 0x12345;
        if (st->psrc[i] != v || st->pkey[i] != key || !(st->mask & 0x40)) {
            float kb = slot == 0x49 ? g_rimBright : g_partBright;
            if (sh) make_shine(st->pm[i], v, rgb, srgb, kb, shine_amt(ride, slot), sa); else make_neutral(st->pm[i], v, kb);
            st->psrc[i] = v; st->pkey[i] = key; st->mask |= 0x40;
            clog("part %02x: car %p uses its own copy of %s (%s)", slot, cri, (char *)v + 0x14, sh ? "base + shine" : "neutral");
        }
        }
        return st->pm[i];
    }
}
static int g_skin2Shine;              /* ini Skin2Shine: 1 = the 2nd skin material slot also gets the shine */
/* 2nd skin slot (hash 0x5be72f1c): window trim / rubber in races take it - neutral copy of the body material, no shine tint */
static BYTE *paint_mat2(BYTE *cri, DWORD off)
{
    BYTE *v = *(BYTE **)(cri + off), *mat = *(BYTE **)(cri + 0x384), *ride = *(BYTE **)(cri + 4); CriState *st; DWORD rgb;
    if (off != 0x384 && v != mat) return paint_mat_(cri, off);          /* +0x39c holding the trunk audio material */
    if (!v || !readable(v, 0xa8) || !readable(ride, 0x590 + 170 * 4) || !g_kindOn[K_PAINT] || !mine(ride)) return v;
    if (!custom_for(ride, 0x3f, &rgb) && !custom_for(ride, SHINE_SLOT, &rgb)) return v;
    st = cri_state(cri);
    if (st->psrc[7] != v || st->pkey[7] != 0x2222 || !(st->mask & 0x40)) {
        make_neutral(st->pm[7], v, 0.f); st->psrc[7] = v; st->pkey[7] = 0x2222; st->mask |= 0x40;
        clog("2nd skin slot: car %p uses a neutral copy of %s", cri, (char *)v + 0x14);
    }
    return st->pm[7];
}
static BYTE *__cdecl paint_mat(BYTE *cri, DWORD off)
{
    if (off & 0x10000) {
        off &= 0xffff;
        if (!g_skin2Shine) { __try { return paint_mat2(cri, off); } __except (EXCEPTION_EXECUTE_HANDLER) { return *(BYTE **)(cri + off); } }
    }
    __try { return paint_mat_(cri, off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) clog("paint material hook: exception (cri %p)", cri); }
    return *(BYTE **)(cri + off);
}
static int hook_matread(DWORD at, const BYTE *expect, DWORD flag)
{
    BYTE *t, dst, base; DWORD old, disp; int n = 0, k;
    if (memcmp((void *)at, expect, 6)) { clog("paint material read at %08lx: unexpected bytes - not hooked", at); return 0; }
    dst = (expect[1] >> 3) & 7; base = expect[1] & 7; disp = *(DWORD *)(expect + 2);
    k = dst == 0 ? 0 : dst == 2 ? 4 : dst == 1 ? 8 : -1;            /* saved eax / edx / ecx; -1 = another register */
    if (base == 4 || dst == 4) return 0;
    t = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    t[n++] = 0x9C; t[n++] = 0x51; t[n++] = 0x52; t[n++] = 0x50;      /* pushfd, push ecx, edx, eax */
    t[n++] = 0x68; *(DWORD *)(t + n) = disp | flag; n += 4;          /* push offset (| 0x10000 = 2nd skin slot) */
    t[n++] = (BYTE)(0x50 + base);                                     /* push cri */
    t[n++] = 0xE8; *(DWORD *)(t + n) = (DWORD)paint_mat - (DWORD)(t + n + 4); n += 4;
    t[n++] = 0x83; t[n++] = 0xC4; t[n++] = 0x08;                      /* add esp, 8 */
    if (k >= 0) { t[n++] = 0x89; t[n++] = 0x44; t[n++] = 0x24; t[n++] = (BYTE)k; }   /* mov [esp+k], eax */
    else { t[n++] = 0x89; t[n++] = (BYTE)(0xC0 | dst); }                              /* mov dst, eax */
    t[n++] = 0x58; t[n++] = 0x5A; t[n++] = 0x59; t[n++] = 0x9D;      /* pop eax, edx, ecx, popfd */
    t[n++] = 0xE9; *(DWORD *)(t + n) = at + 6 - (DWORD)(t + n + 4); n += 4;
    VirtualProtect((void *)at, 6, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE *)at = 0xE9; *(DWORD *)(at + 1) = (DWORD)t - (at + 5); *(BYTE *)(at + 5) = 0x90;
    VirtualProtect((void *)at, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, 6);
    return 1;
}
static void hook_matreads(void)
{
    static const struct { DWORD at; BYTE b[6]; } S[] = {   /* 0x6308e8 / 0x6308f1 feed material slot 0x5be72f1c (2nd skin slot) */
        { 0x61740b, { 0x8b, 0x96, 0x84, 0x03, 0, 0 } }, { 0x622f0d, { 0x8b, 0x8a, 0x84, 0x03, 0, 0 } },
        { 0x626fb5, { 0x8b, 0x88, 0x84, 0x03, 0, 0 } }, { 0x630662, { 0x8b, 0x88, 0x84, 0x03, 0, 0 } },
        { 0x6308c1, { 0x8b, 0x97, 0x84, 0x03, 0, 0 } }, { 0x6308f1, { 0x8b, 0x87, 0x84, 0x03, 0, 0 } },
        { 0x6308e8, { 0x8b, 0x97, 0x9c, 0x03, 0, 0 } }, { 0x630ca5, { 0x8b, 0x91, 0x9c, 0x03, 0, 0 } },
        { 0x61791d, { 0x8b, 0x93, 0x90, 0x03, 0, 0 } }, { 0x62772c, { 0x8b, 0x88, 0x90, 0x03, 0, 0 } },
        { 0x624c0a, { 0x8b, 0x80, 0x94, 0x03, 0, 0 } }, { 0x62e008, { 0x8b, 0xb7, 0x98, 0x03, 0, 0 } },
        { 0x622ee8, { 0x8b, 0x82, 0xa0, 0x03, 0, 0 } }, { 0x622f23, { 0x8b, 0x82, 0xa4, 0x03, 0, 0 } },
        { 0x622f3b, { 0x8b, 0x82, 0xa8, 0x03, 0, 0 } },
    };
    int i, n = 0;
    for (i = 0; i < (int)(sizeof S / sizeof S[0]); i++) n += hook_matread(S[i].at, S[i].b, (S[i].at == 0x6308e8 || S[i].at == 0x6308f1) ? 0x10000 : 0);
    clog("paint materials: %d of %d reads hooked", n, (int)(sizeof S / sizeof S[0]));
}
static long g_cNeon;
/* E2: cars the game drew since the last frame callback. Remembered car pointers (purge re-runs, decal re-tints) are used
   only while their car is in this table: a car the shop freed is never drawn again, so it is never written to. */
typedef struct { BYTE *cri, *ride; long frame; } LiveCar;
static LiveCar g_live[32]; static long g_cDraw;   /* frame callbacks so far */
static void live_note(BYTE *cri, BYTE *ride)
{
    int i, f = -1; long fr = g_cDraw;
    for (i = 0; i < 32; i++) {
        if (g_live[i].cri == cri) { g_live[i].ride = ride; g_live[i].frame = fr; return; }
        if (f < 0 && fr - g_live[i].frame > 1) f = i;
    }
    if (f < 0) { f = 0; for (i = 1; i < 32; i++) if (g_live[i].frame < g_live[f].frame) f = i; }
    g_live[f].cri = cri; g_live[f].ride = ride; g_live[f].frame = fr;
}
static int cri_live(BYTE *cri)      /* drawn since the last frame callback (g_cDraw was bumped once since) */
{
    int i; for (i = 0; i < 32; i++) if (g_live[i].cri == cri && g_cDraw - g_live[i].frame == 1) return 1;
    return 0;
}
static int ride_recent(BYTE *ride)
{
    int i; for (i = 0; i < 32; i++) if (g_live[i].ride == ride && g_cDraw - g_live[i].frame <= 1) return 1;
    return 0;
}
static int ride_live(BYTE *ride)
{
    int i; for (i = 0; i < 32; i++) if (g_live[i].ride == ride && g_cDraw - g_live[i].frame == 1) return 1;
    return 0;
}
static void neon_fix_(BYTE *cri)
{
    BYTE *ride = *(BYTE **)(cri + 4); CriState *st; int g; DWORD rgb = 0;
    live_note(cri, ride);
    if (!readable(ride, 0x590 + 170 * 4)) return;
    g_cNeon++;
    note_ride(ride);
    recomposite_due();
    st = cri_state(cri);
    if (!st->mask && !mine(ride)) return;           /* someone else's car, never touched */
    /* E9: decal spots checked every frame on our drawn cars - the decal list swaps decals on the shown car without a paint
       re-read, so it kept the tinted copies of older decals (4 repeated while scrolling) until a colour change */
    if (mine(ride) && g_kindOn[K_PAINT]) { __try { decal_fix(cri, ride); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) clog("decal colour: frame fix exception 0x%08lx", GetExceptionCode()); } }
    for (g = 0; g < 5; g++) {
        int slot = g < 4 ? 0x9a + g : 0x9f, on = g_kindOn[g < 4 ? K_NEON : K_LIGHT] && custom_for(ride, slot, &rgb);
        BYTE r = (BYTE)(rgb >> 16), gg = (BYTE)(rgb >> 8), b = (BYTE)rgb, *p = part_of(ride, slot);
        if (on) {
            if (g < 4) {
                BYTE *base = cri + 0x34c + g * 12, *dl = base + 4, *o2 = base + 8, *out = base + 12;
                base[0] = r; base[1] = gg; base[2] = b; dl[0] = dl[1] = dl[2] = 0;
                o2[0] = r; o2[1] = gg; o2[2] = b; out[0] = r; out[1] = gg; out[2] = b;
            } else { BYTE *h = cri + 0x380; h[0] = r; h[1] = gg; h[2] = b; }
            st->mask |= 1 << g;
        } else if (st->mask & (1 << g)) {          /* custom removed: back to the part's own colour */
            st->mask &= ~(1 << g);
            if (g < 4) NeonCol(cri, NULL, p, cri + 0x34c + g * 12, cri + 0x350 + g * 12);
            else {
                BYTE *h = cri + 0x380; DWORD *a;
                if (!p) *(DWORD *)h = 0x90aa90;
                else {
                    a = o_attr(p, NULL, H_RED, NULL);   h[0] = a ? (BYTE)a[1] : 0;
                    a = o_attr(p, NULL, H_GREEN, NULL); h[1] = a ? (BYTE)a[1] : 0;
                    a = o_attr(p, NULL, H_BLUE, NULL);  h[2] = a ? (BYTE)a[1] : 0; h[3] = 0;
                }
            }
        }
    }
}
static void __cdecl neon_fix(BYTE *cri)
{
    if (!g_enabled) return;
    __try { neon_fix_(cri); }
    __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) clog("neon hook: exception 0x%08lx (cri %p)", GetExceptionCode(), cri); }
}
static BYTE *g_neonTramp;
__declspec(naked) static void h_neon(void)
{
    __asm {
        pushad
        push ecx
        call neon_fix
        add esp, 4
        popad
        jmp dword ptr [g_neonTramp]     /* the game's neon draw (thiscall, ret 0xc) */
    }
}

/* HUD colours: globals filled from the FE ride when a race starts */
static void hud_fix(void)
{
    static const DWORD G[3][3] = { { 0x839bfc, 0x839bf8, 0x839bf4 }, { 0x839c0c, 0x839c08, 0x839c04 }, { 0x839c1c, 0x839c18, 0x839c14 } };
    int i; DWORD rgb;
    if (!g_kindOn[K_HUD]) return;
    for (i = 0; i < NSLOT; i++) {
        const DWORD *g;
        if (SLOTS[i].kind != K_HUD || SLOTS[i].arg > 2 || !custom_for((BYTE *)FE_RIDE, SLOTS[i].slot, &rgb)) continue;
        g = G[SLOTS[i].arg];
        *(DWORD *)g[0] = (rgb >> 16) & 0xFF; *(DWORD *)g[1] = (rgb >> 8) & 0xFF; *(DWORD *)g[2] = rgb & 0xFF;
    }
}

/* ---------------- picker ---------------- */
static int g_open, g_row, g_sel;                 /* row 0 part, 1 hue, 2 saturation, 3 brightness, 4 custom on/off */
static float g_h, g_s = 1.f, g_v = 1.f;
static BYTE *g_pRide; static char g_pCar[32];
static DWORD g_dirtyAt; static int g_dirty;
static int g_mx = -1, g_my = -1, g_mDown, g_mDrag;   /* mouse in back buffer pixels; drag 1 = SV square, 2 = hue bar */
static DWORD g_mMoved;
static float g_bx, g_by, g_bsz, g_hx, g_hy, g_hw, g_hh; static int g_hHoriz;   /* last drawn SV square + hue bar (game thread) */
/* shop mode: the picker sits in the shop's colour-dot box, opened by the shop's own button (Install -> "Custom color") */
static int g_shopMode, g_shopSlot = -1, g_inShop; static BYTE *g_nav, *g_anyNav;
static BYTE *g_nlOv; static int g_nlFocus;     /* neon list CUSTOM row (see nl_add) */
static DWORD g_nlLangHash = 0xFFFFFFFF;           /* language hash of the part under the CUSTOM row (its name text) */
static char *__cdecl nl_lang_text(void);
static int g_boxL, g_boxT, g_boxR, g_boxB;       /* the box in 1/10000 of the client area, for the mouse hook */
#ifdef NFSU2CC_SHIP
static char g_shopPk[256] = "UI_Paint.fng,UI_GenericParts_Browser.fng";
#else
static char g_shopPk[256] = "UI_Paint.fng,UI_NeonPartsOverlay.fng,UI_CustomNeonMain.fng,UI_ChooseCustomHUD.fng,UI_CustomHUDOverlay.fng,UI_GenericParts_Browser.fng";
#endif
static HWND g_gameWnd; static HINSTANCE g_self;

static void hsv2rgb(float h, float s, float v, float *r, float *g, float *b)
{
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.f, 2.f) - 1)), m = v - c;
    int i = (int)(h / 60.f) % 6;
    float R[6] = { c, x, 0, 0, x, c }, Gg[6] = { x, c, c, x, 0, 0 }, B[6] = { 0, 0, x, c, c, x };
    if (i < 0) i = 0;
    *r = R[i] + m; *g = Gg[i] + m; *b = B[i] + m;
}
static DWORD hsv_rgb(float h, float s, float v)
{
    float r, g, b; hsv2rgb(h, s, v, &r, &g, &b);
    return ((DWORD)(r * 255 + .5f) << 16) | ((DWORD)(g * 255 + .5f) << 8) | (DWORD)(b * 255 + .5f);
}
static void rgb_hsv(DWORD c, float *h, float *s, float *v)
{
    float r = ((c >> 16) & 255) / 255.f, g = ((c >> 8) & 255) / 255.f, b = (c & 255) / 255.f;
    float mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
    *v = mx; *s = mx > 0 ? d / mx : 0;
    if (d <= 0) return;                                          /* grey: keep the hue the user had */
    if (mx == r) *h = 60.f * fmodf((g - b) / d, 6.f);
    else if (mx == g) *h = 60.f * ((b - r) / d + 2);
    else *h = 60.f * ((r - g) / d + 4);
    if (*h < 0) *h += 360.f;
}
/* the game's own colour of a slot on this ride */
static DWORD game_rgb(BYTE *ride, int slot)
{
    BYTE *p = part_of(ride, slot); DWORD *a; DWORD r, g, b;
    int n = 0;
    if (!p) return 0x808080;
    a = o_attr(p, NULL, H_RED, NULL); r = a ? a[1] & 255 : 0; n += a != NULL;
    a = o_attr(p, NULL, H_GREEN, NULL); g = a ? a[1] & 255 : 0; n += a != NULL;
    a = o_attr(p, NULL, H_BLUE, NULL); b = a ? a[1] & 255 : 0; n += a != NULL;
    /* parts without a colour of their own (decals, picture vinyls, purge): the picker starts from white, not black */
    if (!n && ((slot >= 0x234 && slot <= 0x243) || (slot >= 0x261 && slot <= 0x298) || (slot & 0xFF) == 0xa2)) return 0xFFFFFF;
    return (r << 16) | (g << 8) | b;
}
/* slots the picker shows on this ride: the kind is on and the car has a part there */
static int slot_ok(BYTE *ride, int i) { return g_kindOn[SLOTS[i].kind] && (SLOTS[i].kind != K_NEON || g_neonUnlocked) && part_of(SLOTS[i].kind == K_HUD ? (BYTE *)FE_RIDE : ride, SLOTS[i].slot) != NULL; }
static BYTE *slot_ride(int i) { return SLOTS[i].kind == K_HUD ? (BYTE *)FE_RIDE : g_pRide; }
static void load_sel(void)
{
    Entry *e; BYTE *r = slot_ride(g_sel); DWORD c;
    EnterCriticalSection(&g_cs);
    e = find_r(g_pCar, r, SLOTS[g_sel].slot);
    c = e && (IS_PIC(SLOTS[g_sel].slot) || 0 != part_hash(part_of(r, SLOTS[g_sel].slot))) ? e->rgb : IS_PIC(SLOTS[g_sel].slot) ? 0xFFFFFF : game_rgb(r, SLOTS[g_sel].slot);
    if (IS_PIC(SLOTS[g_sel].slot) && PIC_BROWSE()) c = g_browseOn ? g_browseCol : 0xFFFFFF;
    LeaveCriticalSection(&g_cs);
    rgb_hsv(c, &g_h, &g_s, &g_v);
}
static int sel_custom(void)
{
    Entry *e; BYTE *r = slot_ride(g_sel); int on;
    EnterCriticalSection(&g_cs);
    if (IS_PIC(SLOTS[g_sel].slot) && PIC_BROWSE()) { LeaveCriticalSection(&g_cs); return g_browseOn; }
    e = find_r(g_pCar, r, SLOTS[g_sel].slot); on = e && (IS_PIC(SLOTS[g_sel].slot) || 0 != part_hash(part_of(r, SLOTS[g_sel].slot)));
    LeaveCriticalSection(&g_cs);
    return on;
}
static DWORD g_lastPick; static int g_lastPickOk;      /* E1: the colour the player picked last (any part) */
static void set_custom(int on)
{
    Entry *e; BYTE *r = slot_ride(g_sel); int slot = SLOTS[g_sel].slot;
    if (on) { g_lastPick = hsv_rgb(g_h, g_s, g_v); g_lastPickOk = 1; }
    if (IS_PIC(slot) && PIC_BROWSE()) { pic_browse_set(on, hsv_rgb(g_h, g_s, g_v)); return; }
    EnterCriticalSection(&g_cs);
    if (on) {
        e = make_r(g_pCar, r, slot);
        if (e) { if (!IS_VKEY(e->slot) && !IS_PIC(e->slot)) e->part = part_hash(part_of(r, slot)); e->rgb = hsv_rgb(g_h, g_s, g_v); e->seq = ++g_seq; }
    } else if ((e = find_r(g_pCar, r, slot))) { *e = g_e[--g_ne]; }
    LeaveCriticalSection(&g_cs);
    if (SLOTS[g_sel].kind == K_PAINT || SLOTS[g_sel].kind == K_SHINE) InterlockedIncrement(&g_ver);
    if (SLOTS[g_sel].kind == K_PURGE) InterlockedIncrement(&g_purgeVer);
    g_dirty = 1; g_dirtyAt = GetTickCount();
}
static void set_slot(const char *car, BYTE *r, int slot, DWORD rgb, int on)
{
    Entry *e; int i;
    if (IS_PIC(slot) && PIC_BROWSE()) { pic_browse_set(on, rgb); return; }
    EnterCriticalSection(&g_cs);
    if (on) {
        e = make_r(car, r, slot);
        if (e) { if (!IS_VKEY(e->slot) && !IS_PIC(e->slot)) e->part = part_hash(part_of(r, slot)); e->rgb = rgb & 0xFFFFFF; e->seq = ++g_seq; }
    } else if ((e = find_r(car, r, slot))) { *e = g_e[--g_ne]; }
    LeaveCriticalSection(&g_cs);
    for (i = 0; i < NSLOT; i++) if (SLOTS[i].slot == slot && (SLOTS[i].kind == K_PAINT || SLOTS[i].kind == K_SHINE || SLOTS[i].kind == K_GLOSS)) InterlockedIncrement(&g_ver);
    if ((slot & 0xF00) == 0x500) InterlockedIncrement(&g_ver);
    g_dirty = 1; g_dirtyAt = GetTickCount();
}
static int step_sel(int dir)
{
    int i, k = g_sel;
    for (i = 0; i < NSLOT; i++) { k = (k + dir + NSLOT) % NSLOT; if (SLOTS[k].kind != K_GLOSS && SLOTS[k].kind != K_SHINE && slot_ok(g_pRide, k)) return k; }
    return g_sel;
}
static BYTE *pick_ride(void)
{
    int i; DWORD now = GetTickCount(), best = 0xFFFFFFFF; BYTE *r = NULL;
    for (i = 0; i < 8; i++) if (g_seen[i].ride && now - g_seen[i].seen < 1000 && mine(g_seen[i].ride)) {
        if (now - g_seen[i].seen < best) { best = now - g_seen[i].seen; r = g_seen[i].ride; }
    }
    if (!r) r = *(int *)FLOW == 3 ? (BYTE *)FE_RIDE : player_ride();
    return r;
}
/* front end: open the picker on the colour slot the player touched last in the shop */
static BYTE *g_lastParts[NSLOT]; static int g_lastTouched = -1;
static void watch_fe(void)
{
    int i;
    if (*(int *)FLOW != 3) return;
    for (i = 0; i < NSLOT; i++) {
        BYTE *p = part_of((BYTE *)FE_RIDE, SLOTS[i].slot);
        if (g_lastParts[i] && p != g_lastParts[i]) g_lastTouched = i;
        g_lastParts[i] = p;
    }
}
static void open_picker(void)
{
    const char *c;
    g_pRide = pick_ride();
    if (!g_pRide || !(c = car_name(g_pRide))) { clog("picker: no car found"); return; }
    strncpy(g_pCar, c, 31); g_pCar[31] = 0;
    if (g_lastTouched >= 0 && slot_ok(g_pRide, g_lastTouched)) g_sel = g_lastTouched;
    else if (!slot_ok(g_pRide, g_sel)) g_sel = step_sel(1);
    g_row = 1; g_open = 1;
    load_sel();
    clog("picker opened: car %s ride %p, slot %s", g_pCar, g_pRide, SLOTS[g_sel].name);
}

/* key / mouse events from the low-level hooks (own thread) -> handled in the game thread */
#define QN 64
static volatile LONG g_qw, g_qr; static DWORD g_q[QN];
static void qpush(DWORD v) { LONG w = g_qw; if (w - g_qr < QN) { g_q[w % QN] = v; InterlockedIncrement(&g_qw); } }
/* Input comes from the game's own readers, not from Windows hooks (DirectInput eats keys/clicks before low-level hooks):
   - mouse: 0x5bf860 polls the DirectInput mouse (this +0x21 left held, +0x22 pressed, +0x23 released) and the cursor;
     a click that starts on the picker is taken by the picker and hidden from the menu until the button is released.
   - keyboard: the game reads [0x86e5c0] with IDirectInputDevice8::GetDeviceData; the picker's keys are taken out of
     the returned events while it is open (the key-up too, so the menu never sees half a key). */
#define A_MOUSEPOLL 0x5bf860
#define KBD_DEV     0x86e5c0
typedef struct { DWORD ofs, data, ts, seq, app; } DIOD;
typedef HRESULT (__stdcall *PFN_GDD)(void *, DWORD, BYTE *, DWORD *, DWORD);
static PFN_GDD o_gdd; static void **g_gddVt;
static int (__fastcall *o_mpoll)(BYTE *, void *);
static BYTE g_dikHeld[256], g_dikTaken[256];
static int g_mBlock, g_mWas, g_ownCursor; static DWORD g_repDik, g_repNext, g_pickDik;
static WNDPROC g_oldWndProc;
static DWORD dik_vk(DWORD dik)
{
    switch (dik) {
    case 0xC8: return VK_UP; case 0xD0: return VK_DOWN; case 0xCB: return VK_LEFT; case 0xCD: return VK_RIGHT;
    case 0xC9: return VK_PRIOR; case 0xD1: return VK_NEXT; case 0xC7: return VK_HOME; case 0xCF: return VK_END;
    case 0xD3: return VK_DELETE; case 0x1C: return VK_RETURN; case 0x01: return VK_ESCAPE; case 0x0E: return VK_BACK;
    case 0x0F: return VK_TAB; case 0x39: return VK_SPACE;
    }
    return 0;
}
static int picker_wants(DWORD vk)           /* keys the open picker takes (shop mode: Enter/Esc stay with the shop) */
{
    if (!g_open) return 0;
    switch (vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END: case VK_DELETE: return 1;
    case VK_RETURN: case VK_ESCAPE: case VK_BACK: case VK_TAB: case VK_SPACE: return !g_shopMode;
    }
    return 0;
}
static int g_turnOn = 1; static int turn_active(void);
static int g_driveOrbit;   /* ini DriveOrbit: K / L orbit camera while driving (0 = off, the release default until it works) */
static DWORD shift_flag(void) { return (g_dikHeld[0x2A] || g_dikHeld[0x36]) ? 0x20000 : 0; }
/* which DirectInput devices are keyboards (asked once per device: GetCapabilities, dwDevType & 0xFF == 0x13) */
static void *g_kbDev[8]; static BYTE g_kbIs[8]; static int g_kbN;
static int is_kbd(void *dev)
{
    int i; DWORD caps[11]; HRESULT (__stdcall *gc)(void *, DWORD *);
    for (i = 0; i < g_kbN; i++) if (g_kbDev[i] == dev) return g_kbIs[i];
    memset(caps, 0, sizeof caps); caps[0] = sizeof caps;
    gc = (HRESULT (__stdcall *)(void *, DWORD *))(*(void ***)dev)[3];
    i = g_kbN < 8 ? g_kbN++ : 7;
    g_kbDev[i] = dev; g_kbIs[i] = gc(dev, caps) >= 0 && (caps[2] & 0xFF) == 0x13;
    clog("input device %p: %s (type %08lx)", dev, g_kbIs[i] ? "keyboard" : "not a keyboard", caps[2]);
    return g_kbIs[i];
}
static HRESULT __stdcall h_gdd(void *dev, DWORD cb, BYTE *buf, DWORD *inout, DWORD flags)
{
    HRESULT hr = o_gdd(dev, cb, buf, inout, flags);
    DWORD i, n, k = 0;
    if (hr < 0 || !buf || !inout || cb < 8 || (flags & 1) || !g_enabled || !is_kbd(dev)) return hr;   /* 1 = DIGDD_PEEK */
    n = *inout;
    for (i = 0; i < n; i++) {
        BYTE *e = buf + i * cb; DWORD dik = ((DIOD *)e)->ofs & 0xFF, down = (((DIOD *)e)->data & 0x80) != 0, vk = dik_vk(dik), take = 0;
        g_dikHeld[dik] = (BYTE)down;
        if (g_pickDik && dik == g_pickDik) { if (down) qpush(g_key | 0x10000); take = 1; }
        else if ((dik == 0x25 || dik == 0x26) && (down ? (turn_active() || (g_turnOn && g_driveOrbit && *(int *)FLOW == 6)) : g_dikTaken[dik])) { g_dikTaken[dik] = (BYTE)down; take = 1; }   /* K / L: turn the car */
        else if (down && vk && picker_wants(vk)) {
            qpush(vk | shift_flag()); g_dikTaken[dik] = 1; take = 1;
            g_repDik = dik; g_repNext = GetTickCount() + 350;
        } else if (!down && g_dikTaken[dik]) { g_dikTaken[dik] = 0; take = 1; if (g_repDik == dik) g_repDik = 0; }
        if (!take) { if (k != i) memmove(buf + k * cb, e, cb); k++; }
    }
    *inout = k;
    return hr;
}
static LRESULT CALLBACK h_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if ((m == WM_KEYDOWN || m == WM_KEYUP) && picker_wants((DWORD)wp)) return 0;   /* the game's own text keys */
    return CallWindowProcA(g_oldWndProc, w, m, wp, lp);
}
static int game_front(void)
{
    DWORD pid = 0; HWND w = GetForegroundWindow();
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) { g_gameWnd = w; return 1; }
    return 0;
}
static void input_setup(void)                /* game thread: hook the keyboard device once it exists, subclass the window */
{
    void *kb = *(void **)KBD_DEV;
    if (kb && readable(kb, 4)) {
        void **vt = *(void ***)kb;
        if (vt != g_gddVt && readable(vt, 11 * 4) && vt[10] != (void *)h_gdd) {
            DWORD old;
            if (VirtualProtect(&vt[10], 4, PAGE_EXECUTE_READWRITE, &old)) {
                o_gdd = (PFN_GDD)vt[10]; vt[10] = (void *)h_gdd; VirtualProtect(&vt[10], 4, old, &old);
                g_gddVt = vt; clog("keyboard: picker keys hooked (device %p)", kb);
            }
        }
    }
    if (!g_oldWndProc && g_gameWnd) {
        g_oldWndProc = (WNDPROC)SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)h_wndproc);
        clog("window keys hooked (%p)", g_gameWnd);
    }
}
/* neon settings panel (neon shop screens): rects in back-buffer pixels, set when drawn (0 width = not shown) */
static float g_nsBtn[4], g_nsPan[4]; static int g_nsW, g_nsH, g_nsOver, g_nsDrag = -1; static volatile int g_nsPress, g_nsHeld;
static int in_r4(const float *r, float x, float y) { return r[2] > 0 && x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3]; }
static int __fastcall h_mpoll(BYTE *self, void *edx)
{
    int r = o_mpoll(self, edx), raw;
    POINT p; RECT rc;
    if (!g_enabled) return r;
    raw = self[0x21] != 0;
    if (game_front() && GetCursorPos(&p) && ScreenToClient(g_gameWnd, &p) && GetClientRect(g_gameWnd, &rc) && rc.right > 0 && rc.bottom > 0) {
        int x = p.x * 10000 / rc.right, y = p.y * 10000 / rc.bottom;
        if (x != g_mx || y != g_my) { g_mx = x; g_my = y; g_mMoved = GetTickCount(); }
    }
    if (g_open && raw && !g_mWas && (!g_shopMode || (g_mx >= g_boxL && g_mx <= g_boxR && g_my >= g_boxT && g_my <= g_boxB))) g_mBlock = 1;
    {   /* neon settings panel: its clicks are ours, the game sees none */
        float fx = g_mx / 10000.f * g_nsW, fy = g_my / 10000.f * g_nsH;
        g_nsOver = g_nsW > 0 && (in_r4(g_nsBtn, fx, fy) || in_r4(g_nsPan, fx, fy));
        if (g_nsOver && raw && !g_mWas) { g_nsPress = 1; g_mBlock = 1; }
        g_nsHeld = raw;
    }
    g_mDown = g_mBlock && raw && g_open;
    if (g_mBlock) { self[0x21] = self[0x22] = self[0x23] = 0; if (!raw) g_mBlock = 0; }
    g_ownCursor = g_open && g_shopMode && g_mx >= g_boxL - 30 && g_mx <= g_boxR + 30 && g_my >= g_boxT - 60 && g_my <= g_boxB + 60;
    if (g_ownCursor || g_nsOver || g_nsDrag >= 0) {            /* the menu sees the cursor far away: the colour dots under the picker get no hover or click */
        *(int *)(self + 4) = -2000; *(int *)(self + 8) = -2000; *(int *)(self + 0x14) = 0; *(int *)(self + 0x18) = 0;
    }
    g_mWas = raw;
    return r;
}
static void key_repeat(void)                 /* DirectInput has no key repeat: repeat held picker keys here */
{
    DWORD now = GetTickCount();
    if (!g_repDik || !g_open) { g_repDik = 0; return; }
    if (!g_dikHeld[g_repDik]) { g_repDik = 0; return; }
    if ((LONG)(now - g_repNext) >= 0) { qpush(dik_vk(g_repDik) | shift_flag()); g_repNext = now + 35; }
}
static int g_rep; static DWORD g_lastKeyT, g_lastKey;
static void handle_key(DWORD ev)
{
    DWORD vk = ev & 0xFFFF; int shift = (ev & 0x20000) != 0, dir; float st;
    if (ev & 0x10000) { if (g_open) { g_open = 0; clog("picker closed"); } else open_picker(); return; }
    if (!g_open) return;
    if (vk == g_lastKey && GetTickCount() - g_lastKeyT < 150) g_rep++; else g_rep = 0;
    g_lastKey = vk; g_lastKeyT = GetTickCount();
    if (g_shopMode) {
        st = (shift ? 5.f : 1.f) * (g_rep > 20 ? 4.f : g_rep > 6 ? 2.f : 1.f);
        switch (vk) {
        case VK_LEFT: case VK_RIGHT: if (g_s < 0.1f || g_v < 0.1f) { g_s = 1.f; g_v = 1.f; } g_h += (vk == VK_RIGHT ? 2.f : -2.f) * st; while (g_h < 0) g_h += 360.f; while (g_h >= 360.f) g_h -= 360.f; break;
        case VK_UP: case VK_DOWN: g_v = min(1.f, max(0.f, g_v + (vk == VK_UP ? 0.01f : -0.01f) * st)); break;
        case VK_HOME: case VK_END: g_s = min(1.f, max(0.f, g_s + (vk == VK_END ? 0.01f : -0.01f) * st)); break;
        case VK_PRIOR: g_sel = step_sel(-1); load_sel(); return;
        case VK_NEXT: g_sel = step_sel(1); load_sel(); return;
        case VK_DELETE: if (sel_custom()) set_custom(0); load_sel(); return;
        default: return;
        }
        set_custom(1);
        return;
    }
    switch (vk) {
    case VK_ESCAPE: g_open = 0; return;
    case VK_UP: g_row = (g_row + 4) % 5; return;
    case VK_DOWN: case VK_TAB: g_row = (g_row + 1) % 5; return;
    case VK_PRIOR: g_sel = step_sel(-1); load_sel(); return;
    case VK_NEXT: g_sel = step_sel(1); load_sel(); return;
    case VK_DELETE: case VK_BACK: if (sel_custom()) set_custom(0); load_sel(); return;
    case VK_RETURN: case VK_SPACE: set_custom(!sel_custom()); load_sel(); return;
    case VK_LEFT: case VK_RIGHT: case VK_HOME: case VK_END: break;
    default: return;
    }
    dir = vk == VK_RIGHT || vk == VK_END ? 1 : -1;
    if (g_row == 0) { g_sel = step_sel(dir); load_sel(); return; }
    if (g_row == 4) { set_custom(dir > 0); load_sel(); return; }
    st = (shift ? 5.f : 1.f) * (g_rep > 20 ? 4.f : g_rep > 6 ? 2.f : 1.f);
    if (vk == VK_HOME || vk == VK_END) st = 1000.f;
    if (g_row == 1) { g_h += dir * 2.f * st; while (g_h < 0) g_h += 360.f; while (g_h >= 360.f) g_h -= 360.f; }
    if (g_row == 2) g_s = min(1.f, max(0.f, g_s + dir * 0.01f * st));
    if (g_row == 3) g_v = min(1.f, max(0.f, g_v + dir * 0.01f * st));
    set_custom(1);
}

/* ---------------- drawing (inside HDToggle's draw callback) ---------------- */
typedef struct { float x, y, z, rhw; DWORD c; float u, v; } V;
static IDirect3DTexture9 *g_font; static IDirect3DDevice9 *g_fontDev;
static float g_gw[96], g_gu[96]; static int g_fontH = 32, g_atlasW = 2048;
/* The game's own menu font (ITC Conduit Medium Italic, "FONT_CONDUITMDITCTT38BI"), read from the player's game files:
   texture = LanguageTextures.bin TPK (entry table chunk 0x33310003, 24 bytes: hash, offset, csize, usize, ..; JDLZ block,
   512x256 A8R8G8B8), glyph table = the FNTF block after "font_conduitmditctt38bi.bin" in LANGUAGES\English.bin
   (+0xa glyph count, +0x24 line height, records at +0x80, 16 bytes: char, w, h, x, y, ?, xoff, yoff, ?, ?, advance). */
typedef struct { short w, h, x, y, xo, yo, adv; } Glyph;
static Glyph g_gg[96]; static int g_gameFont, g_gfLine = 39; static IDirect3DTexture9 *g_gfTex;
static BYTE *read_file(const char *path, DWORD *size)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); BYTE *b; DWORD n = 0;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    *size = GetFileSize(f, NULL); b = (BYTE *)malloc(*size + 16);
    if (b && !ReadFile(f, b, *size, &n, NULL)) { free(b); b = NULL; }
    CloseHandle(f); return b;
}
static int jdlz(const BYTE *src, DWORD slen, BYTE *out, DWORD usize)
{
    DWORD ip = 16, op = 0, f1 = 1, f2 = 1;
    if (slen < 16 || memcmp(src, "JDLZ", 4)) return 0;
    while (ip < slen && op < usize) {
        if (f1 == 1) f1 = src[ip++] | 0x100;
        if (f2 == 1) f2 = src[ip++] | 0x100;
        if (f1 & 1) {
            DWORD ln, t, i;
            if (f2 & 1) { ln = (src[ip + 1] | ((src[ip] & 0xF0) << 4)) + 3; t = (src[ip] & 0x0F) + 1; }
            else { t = (src[ip + 1] | ((src[ip] & 0xE0) << 3)) + 17; ln = (src[ip] & 0x1F) + 3; }
            ip += 2;
            if (t > op) return 0;
            for (i = 0; i < ln && op < usize; i++, op++) out[op] = out[op - t];
            f2 >>= 1;
        } else out[op++] = src[ip++];
        f1 >>= 1;
    }
    return op == usize;
}
static void load_game_font(IDirect3DDevice9 *dev)
{
    char path[MAX_PATH]; DWORD n, i, k; BYTE *b, *px = NULL; int ok = 0;
    static int tried; if (tried) return; tried = 1;
    /* glyph table */
    sprintf(path, "%s\\..\\LANGUAGES\\English.bin", g_dir);
    if ((b = read_file(path, &n))) {
        for (i = 0; i + 32 < n; i++) if (!_strnicmp((char *)b + i, "font_conduitmditctt38bi", 23)) break;
        for (; i + 0x80 < n; i++) if (!memcmp(b + i, "FNTF", 4)) break;
        if (i + 0x80 < n) {
            int cnt = *(WORD *)(b + i + 0xa); g_gfLine = *(int *)(b + i + 0x24); if (g_gfLine < 10 || g_gfLine > 80) g_gfLine = 39;
            for (k = 0; k < (DWORD)cnt && i + 0x90 + k * 16 < n; k++) {
                BYTE *r = b + i + 0x80 + k * 16; int c = *(WORD *)r;
                if (c < 32 || c >= 128) continue;
                g_gg[c - 32].w = r[2]; g_gg[c - 32].h = r[3]; g_gg[c - 32].x = *(short *)(r + 4); g_gg[c - 32].y = *(short *)(r + 6);
                g_gg[c - 32].xo = (signed char)r[9]; g_gg[c - 32].yo = (signed char)r[10]; g_gg[c - 32].adv = *(short *)(r + 14);
                ok++;
            }
        }
        free(b);
    }
    /* texture */
    sprintf(path, "%s\\..\\LANGUAGES\\LanguageTextures.bin", g_dir);
    if (ok > 60 && (b = read_file(path, &n))) {
        for (i = 0; i + 8 < n; i += 4) if (*(DWORD *)(b + i) == 0x33310003) break;
        if (i + 8 < n) {
            DWORD sz = *(DWORD *)(b + i + 4), e;
            for (e = 0; e + 24 <= sz; e += 24) {
                DWORD *en = (DWORD *)(b + i + 8 + e);
                if (en[0] == 0x9583aa1a && en[1] + en[2] <= n && en[3] >= 512 * 256 * 4) {
                    px = (BYTE *)malloc(en[3]);
                    if (px && !jdlz(b + en[1], en[2], px, en[3])) { free(px); px = NULL; }
                    break;
                }
            }
        }
        free(b);
    }
    if (px && SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 512, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_gfTex, NULL))) {
        D3DLOCKED_RECT lr; int y, x;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(g_gfTex, 0, &lr, NULL, 0))) {
            for (y = 0; y < 256; y++) for (x = 0; x < 512; x++)
                ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = (((DWORD *)px)[y * 512 + x] & 0xFF000000) | 0xFFFFFF;
            IDirect3DTexture9_UnlockRect(g_gfTex, 0);
            g_gameFont = 1;
        }
    }
    if (px) free(px);
    clog("picker font: %s", g_gameFont ? "the game's own menu font (Conduit italic)" : "game font not found - using Arial");
}
static void make_font(IDirect3DDevice9 *dev)
{
    HDC dc = CreateCompatibleDC(NULL); BITMAPINFO bi; void *bits; HBITMAP bm; HFONT fo; int i, x = 0; D3DLOCKED_RECT lr;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = g_atlasW; bi.bmiHeader.biHeight = -g_fontH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0); SelectObject(dc, bm);
    fo = CreateFontA(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
    SelectObject(dc, fo); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    memset(bits, 0, g_atlasW * g_fontH * 4);
    for (i = 0; i < 96; i++) {
        char ch = (char)(32 + i); SIZE s; GetTextExtentPoint32A(dc, &ch, 1, &s);
        g_gu[i] = (float)x; g_gw[i] = (float)s.cx; TextOutA(dc, x, 3, &ch, 1); x += s.cx + 2;
    }
    if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, g_atlasW, g_fontH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_font, NULL)) &&
        SUCCEEDED(IDirect3DTexture9_LockRect(g_font, 0, &lr, NULL, 0))) {
        int y, xx;
        for (y = 0; y < g_fontH; y++) for (xx = 0; xx < g_atlasW; xx++) {
            DWORD s = ((DWORD *)bits)[y * g_atlasW + xx]; DWORD a = (s >> 8) & 0xFF;
            ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[xx] = (a << 24) | 0xFFFFFF;
        }
        IDirect3DTexture9_UnlockRect(g_font, 0);
    }
    DeleteObject(fo); DeleteObject(bm); DeleteDC(dc); g_fontDev = dev;
    load_game_font(dev);
}
static void solid(IDirect3DDevice9 *d)
{
    IDirect3DDevice9_SetTexture(d, 0, NULL);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
}
static void quad4(IDirect3DDevice9 *d, float x, float y, float w, float h, DWORD tl, DWORD tr, DWORD bl, DWORD br)
{
    V q[4] = { {x,y,0,1,tl,0,0}, {x+w,y,0,1,tr,0,0}, {x,y+h,0,1,bl,0,0}, {x+w,y+h,0,1,br,0,0} };
    solid(d);
    IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}
static void quad(IDirect3DDevice9 *d, float x, float y, float w, float h, DWORD c) { quad4(d, x, y, w, h, c, c, c, c); }
static void frame(IDirect3DDevice9 *d, float x, float y, float w, float h, float t, DWORD c)
{ quad(d, x, y, w, t, c); quad(d, x, y + h - t, w, t, c); quad(d, x, y, t, h, c); quad(d, x + w - t, y, t, h, c); }
#define GF_K 0.80f
static float text_w(const char *s, float sc)
{
    float w = 0;
    for (; *s; s++) { int i = *s - 32; if (i >= 0 && i < 96) w += g_gameFont ? g_gg[i].adv * sc * GF_K : g_gw[i] * sc; }
    return w;
}
static void text(IDirect3DDevice9 *d, const char *s, float x, float y, float sc, DWORD c)
{
    if (g_gameFont && g_gfTex) {
        float k = sc * GF_K;
        IDirect3DDevice9_SetTexture(d, 0, (IDirect3DBaseTexture9 *)g_gfTex);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        for (; *s; s++) {
            int i = *s - 32; Glyph *g;
            if (i < 0 || i >= 96) continue;
            g = &g_gg[i];
            if (g->w && g->h) {
                float gx = x + g->xo * k, gy = y + g->yo * k, w = g->w * k, h = g->h * k;
                float u0 = g->x / 512.f, u1 = (g->x + g->w) / 512.f, v0 = g->y / 256.f, v1 = (g->y + g->h) / 256.f;
                V q[4] = { {gx,gy,0,1,c,u0,v0}, {gx+w,gy,0,1,c,u1,v0}, {gx,gy+h,0,1,c,u0,v1}, {gx+w,gy+h,0,1,c,u1,v1} };
                IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
            }
            x += g->adv * k;
        }
        return;
    }
    if (!g_font) return;
    IDirect3DDevice9_SetTexture(d, 0, (IDirect3DBaseTexture9 *)g_font);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    for (; *s; s++) {
        int i = *s - 32; float w, u0, u1;
        if (i < 0 || i >= 96) continue;
        w = g_gw[i] * sc; u0 = g_gu[i] / g_atlasW; u1 = (g_gu[i] + g_gw[i]) / g_atlasW;
        {
            V q[4] = { {x,y,0,1,c,u0,0}, {x+w,y,0,1,c,u1,0}, {x,y+g_fontH*sc,0,1,c,u0,1}, {x+w,y+g_fontH*sc,0,1,c,u1,1} };
            IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        }
        x += w;
    }
}
static DWORD argb(DWORD rgb, int a) { return ((DWORD)a << 24) | (rgb & 0xFFFFFF); }
static void hue_bar(IDirect3DDevice9 *d, float x, float y, float w, float h, int vertical)
{
    int i;
    for (i = 0; i < 6; i++) {
        DWORD a = argb(hsv_rgb(i * 60.f, 1, 1), 255), b = argb(hsv_rgb((i + 1) * 60.f - 0.01f, 1, 1), 255);
        if (vertical) quad4(d, x, y + h * i / 6, w, h / 6, a, a, b, b);
        else quad4(d, x + w * i / 6, y, w / 6, h, a, b, a, b);
    }
}
static int g_W, g_H;
static void button(IDirect3DDevice9 *d, float x, float y, float w, float h, float s, int active)
{
    float t = 1.f * s, r = 2.f * s;
    quad(d, x + r, y, w - 2 * r, h, active ? 0xF0323A26 : 0xE0141418);           /* dark body, rounded ends */
    quad(d, x, y + r, r, h - 2 * r, active ? 0xF0323A26 : 0xE0141418); quad(d, x + w - r, y + r, r, h - 2 * r, active ? 0xF0323A26 : 0xE0141418);
    quad(d, x + r, y, w - 2 * r, t, 0xFFD0D0C8); quad(d, x + r, y + h - t, w - 2 * r, t, 0xFFD0D0C8);   /* light outline */
    quad(d, x, y + r, t, h - 2 * r, 0xFFD0D0C8); quad(d, x + w - t, y + r, t, h - 2 * r, 0xFFD0D0C8);
    quad(d, x + t, y + t, r - t, r - t, 0xFFD0D0C8); quad(d, x + w - r, y + t, r - t, r - t, 0xFFD0D0C8);
    quad(d, x + t, y + h - r, r - t, r - t, 0xFFD0D0C8); quad(d, x + w - r, y + h - r, r - t, r - t, 0xFFD0D0C8);
}
#define GAME_GREEN 0xFFC8E870
static float g_tab[4][4], g_shR[4]; static int g_tabsOn, g_tabBase = 0x3f; static float g_glR[4], g_saR[4];   /* g_saR: v1.1 saturation slider */
static int slot_index(int slot);
static int purge_wheels(void), purge_col2(void);
static int purge_tab_slot(int t)          /* purge tabs: 0 Hood, 1 Wheels (keep colour 1/2), 2 Colour 1, 3 Colour 2 (keep the stream) */
{
    int w = t == 1 || (t >= 2 && purge_wheels()), c2 = t == 3 || (t < 2 && purge_col2());
    return w ? (c2 ? PURGE_WHEEL2 : PURGE_WHEEL) : (c2 ? PURGE_HOOD2 : PURGE_HOOD);
}

#define NPRE 10
typedef struct { int used, hasShine; DWORD base, shine, sat; } Pre;     /* sat: v1.1 saturation 0..255 (128 = 100 %) */
static int has_sat_slot(int bs) { int i; for (i = 0; i < NSLOT; i++) if (SLOTS[i].slot == (0x500 | bs)) return 1; return 0; }
static Pre g_pre[NPRE]; static char g_preFile[MAX_PATH]; static float g_preR[NPRE][4], g_saveR[4], g_delR[4]; static int g_delMode;   /* Delete button: next swatch click deletes it */
static void load_pre(void)
{
    FILE *f = fopen(g_preFile, "r"); char ln[128]; unsigned b, sh, sa; int n = 0, nt; char t[16];
    if (!f) return;
    while (n < NPRE && fgets(ln, sizeof ln, f)) {
        if (ln[0] == ';') continue;
        if ((nt = sscanf(ln, "%x %15s %u", &b, t, &sa)) < 1) continue;
        g_pre[n].used = 1; g_pre[n].base = b & 0xFFFFFF;
        g_pre[n].sat = nt >= 3 && sa <= 255 ? sa : 128;                      /* older lines: 100 % */
        g_pre[n].hasShine = t[0] != '-' && sscanf(t, "%x", &sh) == 1; g_pre[n].shine = g_pre[n].hasShine ? sh & 0xFFFFFF : 0;
        n++;
    }
    fclose(f);
}
static void save_pre(void)
{
    FILE *f = fopen(g_preFile, "w"); int i;
    if (!f) { clog("cannot write %s", g_preFile); return; }
    fprintf(f, "; NFSU2_CustomColor - my colours: base RRGGBB, shine RRGGBB or -, saturation 0-255 (128 = 100 %%)\n");
    for (i = 0; i < NPRE; i++) if (g_pre[i].used) {
        if (g_pre[i].hasShine) fprintf(f, "%06X %06X %u\n", g_pre[i].base, g_pre[i].shine, g_pre[i].sat); else fprintf(f, "%06X - %u\n", g_pre[i].base, g_pre[i].sat);
    }
    fclose(f);
}
static int in_r(const float *r, float x, float y) { return x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3]; }
static void pre_click(int i, int shift)
{
    int bs = SLOTS[g_sel].slot & 0xFF, body = (SLOTS[g_sel].kind == K_PAINT || SLOTS[g_sel].kind == K_SHINE) && shine_of(bs); BYTE *r = slot_ride(g_sel);
    if (shift) { if (g_pre[i].used) { memmove(&g_pre[i], &g_pre[i + 1], (NPRE - 1 - i) * sizeof(Pre)); g_pre[NPRE - 1].used = 0; save_pre(); } return; }
    if (!g_pre[i].used) return;
    if (body) {
        set_slot(g_pCar, r, bs, g_pre[i].base, 1);
        set_slot(g_pCar, r, shine_of(bs), g_pre[i].shine, g_pre[i].hasShine);
        if (sat_slot_ok(bs)) set_slot(g_pCar, r, 0x500 | bs, g_pre[i].sat ? g_pre[i].sat : 128, g_pre[i].sat && g_pre[i].sat != 128);   /* v1.1 */
        g_sel = slot_index(bs);
    } else {
        set_slot(g_pCar, r, SLOTS[g_sel].slot, g_pre[i].base, 1);
        if (sat_slot_ok(SLOTS[g_sel].slot)) set_slot(g_pCar, r, 0x500 | bs, g_pre[i].sat ? g_pre[i].sat : 128, g_pre[i].sat && g_pre[i].sat != 128);   /* E13 */
    }
    g_lastPick = g_pre[i].base; g_lastPickOk = 1;
    load_sel();
    clog("my colours: loaded %d (%06lX)", i + 1, g_pre[i].base);
}
static void pre_save(void)
{
    int bs = SLOTS[g_sel].slot & 0xFF, body = (SLOTS[g_sel].kind == K_PAINT || SLOTS[g_sel].kind == K_SHINE) && shine_of(bs); BYTE *r = slot_ride(g_sel); Pre p; Entry *e;
    memset(&p, 0, sizeof p); p.used = 1;
    EnterCriticalSection(&g_cs);
    if (body) {
        e = find_e(g_pCar, bs); p.base = e && 0 != part_hash(part_of(r, bs)) ? e->rgb : game_rgb(r, bs);
        e = find_e(g_pCar, shine_of(bs)); if (e && 0 != part_hash(part_of(r, bs))) { p.hasShine = 1; p.shine = e->rgb; }
        e = find_e(g_pCar, 0x500 | bs); p.sat = e ? e->rgb & 255 : 128;          /* v1.1: the saturation goes with the swatch */
    } else { p.base = hsv_rgb(g_h, g_s, g_v); e = sat_slot_ok(SLOTS[g_sel].slot) ? find_e(g_pCar, 0x500 | bs) : NULL; p.sat = e ? e->rgb & 255 : 128; }
    LeaveCriticalSection(&g_cs);
    memmove(&g_pre[1], &g_pre[0], (NPRE - 1) * sizeof(Pre)); g_pre[0] = p; save_pre();
    clog("my colours: saved %06lX (saturation %d %%)", p.base, (int)(p.sat * 100 / 128.f + 0.5f));
}     /* Base / Shine tab rects (x, y, w, h) in back buffer pixels */
static int slot_index(int slot) { int i; for (i = 0; i < NSLOT; i++) if (SLOTS[i].slot == slot) return i; return 0; }
static void apply_mouse(void)
{
    float mx = g_mx / 10000.f * g_W, my = g_my / 10000.f * g_H; int changed = 0;
    if (!g_mDown) { g_mDrag = 0; return; }
    if (!g_mDrag) {
        if (mx >= g_bx && mx <= g_bx + g_bsz && my >= g_by && my <= g_by + g_bsz) g_mDrag = 1;
        else if (mx >= g_hx && mx <= g_hx + g_hw && my >= g_hy && my <= g_hy + g_hh) { g_mDrag = 2; if (g_s < 0.1f || g_v < 0.1f) { g_s = 1.f; g_v = 1.f; } }   /* hue on white / black: give it full colour */
        else if (g_tabsOn == 1 && g_tabBase == 0x3f && g_glR[2] > 0 && mx >= g_glR[0] - 3 && mx <= g_glR[0] + g_glR[2] + 3 && my >= g_glR[1] - 4 && my <= g_glR[1] + g_glR[3] + 4) g_mDrag = 5;
        else if (g_tabsOn == 1 && g_shR[2] > 0 && mx >= g_shR[0] - 3 && mx <= g_shR[0] + g_shR[2] + 3 && my >= g_shR[1] - 4 && my <= g_shR[1] + g_shR[3] + 4) g_mDrag = 6;
        else if (g_saR[2] > 0 && mx >= g_saR[0] - 3 && mx <= g_saR[0] + g_saR[2] + 3 && my >= g_saR[1] - 4 && my <= g_saR[1] + g_saR[3] + 4) g_mDrag = 7;
        else {
            int t;
            g_mDrag = 3;
            for (t = 0; t < (g_tabsOn == 2 ? 4 : 2) && g_tabsOn; t++)
                if (mx >= g_tab[t][0] && mx <= g_tab[t][0] + g_tab[t][2] && my >= g_tab[t][1] && my <= g_tab[t][1] + g_tab[t][3]) {
                    { g_sel = slot_index(g_tabsOn == 2 ? purge_tab_slot(t) : (t ? shine_of(g_tabBase) : g_tabBase)); load_sel(); }
                }
            if (g_shopMode) {
                for (t = 0; t < NPRE; t++) if (in_r(g_preR[t], mx, my)) {
                    if (g_delMode) {               /* Delete mode: this click deletes the colour, then back to normal */
                        if (g_pre[t].used) { clog("my colours: deleted %d (%06lX)", t + 1, g_pre[t].base); pre_click(t, 1); }
                        g_delMode = 0;
                    } else pre_click(t, g_dikHeld[0x2A] || g_dikHeld[0x36]);
                    break;
                }
                if (in_r(g_saveR, mx, my)) { g_delMode = 0; pre_save(); }
                if (in_r(g_delR, mx, my)) g_delMode = !g_delMode;
                if (g_vpOn && IS_PIC(SLOTS[g_sel].slot)) {          /* E6: picture vinyl presets */
                    if (in_r(g_vpDef, mx, my)) { set_custom(0); load_sel(); clog("shop: %s back to the picture's own colours (Default)", SLOTS[g_sel].name); }
                    for (t = 0; t < g_nvpal; t++) if (g_vpR[t][2] > 0 && in_r(g_vpR[t], mx, my)) {
                        rgb_hsv(g_vpal[t], &g_h, &g_s, &g_v); set_custom(1); clog("shop: %s takes vinyl preset %06lX", SLOTS[g_sel].name, g_vpal[t]); break;
                    }
                }
            }
        }
    }
    if (g_mDrag == 1) { g_s = min(1.f, max(0.f, (mx - g_bx) / g_bsz)); g_v = min(1.f, max(0.f, 1.f - (my - g_by) / g_bsz)); changed = 1; }
    if (g_mDrag == 2) { g_h = min(359.9f, max(0.f, g_hHoriz ? (mx - g_hx) / g_hw * 360.f : (my - g_hy) / g_hh * 360.f)); changed = 1; }
    if (g_mDrag == 5 && g_glR[2] > 0) {
        int gv = (int)(min(1.f, max(0.f, (mx - g_glR[0]) / g_glR[2])) * 255.f + 0.5f);
        set_slot(g_pCar, slot_ride(g_sel), GLOSS_SLOT, (DWORD)gv, 1);
    }
    if (g_mDrag == 6 && g_shR[2] > 0) {
        int av = (int)(min(1.f, max(0.f, (mx - g_shR[0]) / g_shR[2])) * 255.f + 0.5f);
        set_slot(g_pCar, slot_ride(g_sel), 0x300 | g_tabBase, (DWORD)av, 1);
    }
    if (g_mDrag == 7 && g_saR[2] > 0) {                   /* v1.1 saturation: 0..255, 128 = 100 % (snaps to 100 % near the mark) */
        int sv = (int)(min(1.f, max(0.f, (mx - g_saR[0]) / g_saR[2])) * 255.f + 0.5f);
        if (sv > 123 && sv < 133) sv = 128;
        set_slot(g_pCar, slot_ride(g_sel), 0x500 | g_tabBase, (DWORD)sv, 1);
    }
    if (changed) set_custom(1);
}
static void draw_picker(IDirect3DDevice9 *d, int W, int H)
{
    float sc = H / 1080.f * 1.15f, x0 = 60 * sc, y0 = H * 0.16f, pw = 800 * sc, ph = 470 * sc, lh = 46 * sc, ts = 0.95f * sc;
    float lx = x0 + 24 * sc, vx = x0 + 250 * sc, bw = 200 * sc, ly; char buf[128]; int r, on = sel_custom();
    DWORD cur = hsv_rgb(g_h, g_s, g_v), game = game_rgb(slot_ride(g_sel), SLOTS[g_sel].slot);
    const char *rows[5] = { "PART", "HUE", "SATURATION", "BRIGHTNESS", "CUSTOM" };
    g_W = W; g_H = H;
    quad(d, x0, y0, pw, ph, 0xC8101014);
    frame(d, x0, y0, pw, ph, 2 * sc, 0xFF3A3A44);
    sprintf(buf, "CUSTOM COLOUR  -  %s", g_pCar);
    text(d, buf, lx, y0 + 14 * sc, 1.1f * sc, 0xFFFFFFFF);
    for (r = 0; r < 5; r++) {
        DWORD tc = r == g_row ? 0xFFFFD040 : 0xFFC8C8C8;
        ly = y0 + 70 * sc + r * lh;
        if (r == g_row) quad(d, x0 + 8 * sc, ly - 6 * sc, 470 * sc, lh - 4 * sc, 0x40FFD040);
        text(d, rows[r], lx, ly, ts, tc);
        if (r == 0) { sprintf(buf, "<  %s  >", SLOTS[g_sel].label); text(d, buf, vx, ly, ts, 0xFFFFFFFF); }
        else if (r == 4) text(d, on ? "ON" : "OFF  (game colour)", vx, ly, ts, on ? 0xFF60FF80 : 0xFFA0A0A0);
        else {
            float by = ly + 6 * sc, bh = 18 * sc, v = r == 1 ? g_h / 360.f : r == 2 ? g_s : g_v;
            if (r == 1) hue_bar(d, vx, by, bw, bh, 0);
            else if (r == 2) quad4(d, vx, by, bw, bh, argb(hsv_rgb(g_h, 0, g_v), 255), argb(hsv_rgb(g_h, 1, g_v), 255), argb(hsv_rgb(g_h, 0, g_v), 255), argb(hsv_rgb(g_h, 1, g_v), 255));
            else quad4(d, vx, by, bw, bh, 0xFF000000, argb(hsv_rgb(g_h, g_s, 1), 255), 0xFF000000, argb(hsv_rgb(g_h, g_s, 1), 255));
            frame(d, vx + v * bw - 3 * sc, by - 4 * sc, 6 * sc, bh + 8 * sc, 2 * sc, 0xFFFFFFFF);
        }
    }
    /* SV square + vertical hue bar for the mouse */
    g_bsz = 230 * sc; g_bx = x0 + 500 * sc; g_by = y0 + 64 * sc; g_hx = g_bx + g_bsz + 12 * sc;
    g_hy = g_by; g_hw = g_bsz * 0.12f; g_hh = g_bsz; g_hHoriz = 0;
    {
        DWORD hue = argb(hsv_rgb(g_h, 1, 1), 255);
        quad4(d, g_bx, g_by, g_bsz, g_bsz, 0xFFFFFFFF, hue, 0xFFFFFFFF, hue);
        quad4(d, g_bx, g_by, g_bsz, g_bsz, 0x00000000, 0x00000000, 0xFF000000, 0xFF000000);
        frame(d, g_bx + g_s * g_bsz - 5 * sc, g_by + (1 - g_v) * g_bsz - 5 * sc, 10 * sc, 10 * sc, 2 * sc, g_v > 0.5f ? 0xFF000000 : 0xFFFFFFFF);
        hue_bar(d, g_hx, g_by, g_bsz * 0.12f, g_bsz, 1);
        frame(d, g_hx - 2 * sc, g_by + g_h / 360.f * g_bsz - 2 * sc, g_bsz * 0.12f + 4 * sc, 4 * sc, 2 * sc, 0xFFFFFFFF);
    }
    /* new vs game colour */
    ly = y0 + 70 * sc + 5 * lh + 8 * sc;
    text(d, "NEW", lx, ly, 0.8f * sc, 0xFFC8C8C8); quad(d, lx + 70 * sc, ly, 120 * sc, 30 * sc, argb(on ? cur : game, 255));
    text(d, "GAME", lx + 220 * sc, ly, 0.8f * sc, 0xFFC8C8C8); quad(d, lx + 300 * sc, ly, 120 * sc, 30 * sc, argb(game, 255));
    sprintf(buf, "#%06X", on ? cur : game); text(d, buf, g_bx, g_by + g_bsz + 14 * sc, 0.8f * sc, 0xFFFFFFFF);
    if (SLOTS[g_sel].kind == K_NEON && part_of(g_pRide, SLOTS[g_sel].slot) && !(((BYTE *)part_of(g_pRide, SLOTS[g_sel].slot))[5] >> 5))
        text(d, "Fit a neon here in the shop first, then colour it.", lx, ly + 44 * sc, 0.75f * sc, 0xFFFF9060);
    text(d, "Up/Down: row   Left/Right: change (Shift = fast)   PgUp/PgDn: part", lx, y0 + ph - 62 * sc, 0.7f * sc, 0xFF9A9AA6);
    text(d, "Enter: custom on/off   Del: game colour   Mouse: drag in the square   F4: close", lx, y0 + ph - 34 * sc, 0.7f * sc, 0xFF9A9AA6);
    /* mouse cursor (the game hides the Windows one) */
    if (g_mx >= 0 && GetTickCount() - g_mMoved < 4000) {
        float mx = g_mx / 10000.f * W, my = g_my / 10000.f * H;
        quad(d, mx - 8 * sc, my - 1 * sc, 16 * sc, 3 * sc, 0xFFFFFFFF); quad(d, mx - 1 * sc, my - 8 * sc, 3 * sc, 16 * sc, 0xFFFFFFFF);
    }
}

/* ---------------- shop buttons ----------------
   PC nav bar (UI_PC_Customize_Options): 0x54e6e0 lays out the buttons from flag bits (this +0x64 flags, +0x58 package name,
   +0x4c button list; node +0x10 label FEString, +0x18 shown, +0x1c flag). 0x52d330(flag) -> label hash
   (0x01 PC_NAV_QUIT, 0x80 PC_NAV_INSTALL_PAINT, 0x08 ..INSTALL_PART, 0x10 ..INSTALL_VINYL, 0x200 CANCEL_CHANGES).
   Click: 0x55de00, at 0x55de8b eax = node flag; 1 -> quit dialog, others -> 0x52d170(flag) message (install = PAD_ACCEPT).
   0x50c900(FEString, labelHash) sets a label; 0x4ff9d0 (hash in edx) -> text from the language table.
   Here on the shop screens: Quit button -> "Install ..." (does the install), Install button -> "Custom color" / "Preset colors". */
#define A_NAVLABEL 0x52d330
#define A_NAVCLICK 0x55de8b
#define A_LANG     0x4ff9d0
#define A_SETPART  0x631d20
static DWORD H_CC_BTN, H_CC_PRESET;
static char S_CC_BTN[] = "   Custom color", S_CC_PRESET[] = "Preset colors";   /* 3 spaces: room for the rainbow dot */
typedef DWORD (__fastcall *PFN_NavLabel)(BYTE *nav, void *edx, DWORD flag);
static PFN_NavLabel o_navlabel; static void *o_lang, *o_setpart, *g_clickTramp; static DWORD g_clickBack = A_NAVCLICK + 5, g_clickRes;
typedef void (__cdecl *PFN_SetLabel)(void *str, DWORD hash);
static const PFN_SetLabel SetLabel = (PFN_SetLabel)0x50c900;
/* install flag of a shop screen we handle (0 = not one of ours / no quit button to take over) */
/* the generic parts browser (UI_GenericParts_Browser, vtable 0x79d758): +0x58 = selected category item, item +0xc = part slot.
   Its nav buttons are laid out by 0x55edd0 (thiscall browser) - hooked to know the browser object. */
#define A_BROWSERNAV 0x55edd0
static BYTE *g_browser; static void *o_browsernav;
/* E3: the car on the turntable = a car of ours the game drew this frame or the last (the vinyl list's preview car before the
   hidden garage car). Its vinyl layers say whether a picture is shown and which one - no browser-object offsets needed. */
static int is_pic_part(BYTE *part)
{
    int k; DWORD *a = NULL;
    if (!part || !readable(part, 4)) return 0;
    if ((k = vpic_known(part_hash(part))) >= 0) return k;               /* learned from its texture name */
    __try { a = o_attr(part, NULL, 0x564b8cb6, NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { a = NULL; }
    return a && readable(a, 8) && a[1] == 0x40;                          /* full-car vinyl (NUMCOLOURS 0x40) */
}
static BYTE *shown_ride(void)
{
    int i; BYTE *best = NULL;
    if (*(int *)FLOW != 3) return NULL;
    for (i = 0; i < 32; i++) {
        BYTE *r = g_live[i].ride;
        if (!r || g_cDraw - g_live[i].frame > 1 || !readable(r, 0x590 + 170 * 4) || !mine(r)) continue;
        if (!best || (best == g_pRide && r != g_pRide)) best = r;
    }
    return best;
}
static int g_shownVinyl, g_shownPicL = -1;   /* 1 = the car on the turntable has a vinyl at all; layer of its picture */
static BYTE *shown_pic(void)
{
    BYTE *r = shown_ride(), *pp; int L;
    g_shownVinyl = 0; g_shownPicL = -1;
    if (!r) return NULL;
    for (L = 0; L < 4; L++) if ((pp = part_of(r, 0x40 + L))) { g_shownVinyl = 1; if (is_pic_part(pp)) { g_shownPicL = L; return pp; } }
    return NULL;
}
static BYTE *inst_pic(int *lay)            /* the picture vinyl installed on the car (FE ride), its layer */
{
    BYTE *r = (BYTE *)FE_RIDE, *pp; int L;
    if (!readable(r, 0x590 + 170 * 4)) return NULL;
    for (L = 0; L < 4; L++) if ((pp = part_of(r, 0x40 + L)) && is_pic_part(pp)) { if (lay) *lay = L; return pp; }
    return NULL;
}
static void pic_save_inst(void)             /* the browsing colour goes with the installed picture */
{
    int L = 0; BYTE *ip = inst_pic(&L); const char *car = car_name((BYTE *)FE_RIDE); Entry *e;
    if (!ip || !car) return;
    EnterCriticalSection(&g_cs);
    e = find_pic(car, part_hash(ip));
    if (g_browseOn) {
        if (!e && g_ne < MAXE) { e = &g_e[g_ne++]; memset(e, 0, sizeof *e); strncpy(e->car, car, 31); e->slot = 0x240 + L; e->part = part_hash(ip); }
        if (e) { e->rgb = g_browseCol & 0xFFFFFF; e->seq = ++g_seq; }
    } else if (e) *e = g_e[--g_ne];
    LeaveCriticalSection(&g_cs);
    g_dirty = 1; g_dirtyAt = GetTickCount();
}
static void pic_browse_set(int on, DWORD rgb)
{
    BYTE *sp = shown_pic(), *ip = inst_pic(NULL); static DWORD lastLog; static int lastOn = -1;
    g_browseOn = on; if (on) g_browseCol = rgb & 0xFFFFFF;
    if (sp && ip && part_hash(sp) == part_hash(ip)) pic_save_inst();   /* recolouring the installed picture: kept at once */
    InterlockedIncrement(&g_ver);
    if (on != lastOn || GetTickCount() - lastLog > 1500) { lastOn = on; lastLog = GetTickCount(); clog("vinyl list: browsing colour %s", on ? "set" : "off (Default: pictures as drawn)"); }
}
static void pic_browse_enter(void)          /* the list opens: the installed picture's colour is the browsing colour */
{
    BYTE *ip = inst_pic(NULL); const char *car = car_name((BYTE *)FE_RIDE); Entry *e = NULL;
    g_instPic = part_hash(ip);
    EnterCriticalSection(&g_cs); if (ip && car) e = find_pic(car, part_hash(ip)); g_browseOn = e != NULL; g_browseCol = e ? e->rgb : 0; LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_ver);
    clog("vinyl list: installed picture %08lx, browsing colour %s %06lX", g_instPic, g_browseOn ? "=" : "off", g_browseCol);
}
static void pic_install_watch(void)         /* every frame in the list: a new picture installed -> it keeps the browsing colour */
{
    DWORD h;
    if (!PIC_BROWSE()) return;
    h = part_hash(inst_pic(NULL));
    if (h == g_instPic) return;
    g_instPic = h;
    if (h) { pic_save_inst(); clog("vinyl list: picture %08lx installed with %s %06lX", h, g_browseOn ? "colour" : "its own colours (Default)", g_browseCol); }
}
static int vpic_now(void)                   /* the vinyl list shows a picture vinyl now: the selected row's part if known */
{
    if (!g_vinylBrowse) return 0;                       /* rims, spoilers ... in the same browser: never a picture vinyl */
    {   BYTE *pp = shown_pic(); static DWORD lastS = 1; DWORD hs;
        if (shown_ride()) {
            hs = pp ? part_hash(pp) : 0;
            if (hs != lastS) { lastS = hs; { int i2, nd = 0; for (i2 = 0; i2 < 32; i2++) if (g_live[i2].ride && g_cDraw - g_live[i2].frame <= 1) nd++;
              clog("vinyl shop: car on the turntable (ride %p, %d car(s) drawn, picker ride %p) shows %s %08lx", shown_ride(), nd, g_pRide, pp ? "picture vinyl" : "no picture vinyl", hs); } }
            return pp != NULL;
        }
    }

    BYTE *row, *part; int k;
    if (g_browser && readable(g_browser, 0x160) && *(DWORD *)g_browser == 0x79d758 && readable(row = *(BYTE **)(g_browser + 0x15c), 0x30)
        && (part = *(BYTE **)(row + 0x2c)) && readable(part, 4)) {
        static DWORD lastH; int how = 0;
        if ((k = vpic_known(part_hash(part))) < 0) {          /* never painted yet: full-car vinyls (NUMCOLOURS 0x40) are the pictures */
            DWORD *a = NULL; __try { a = o_attr(part, NULL, 0x564b8cb6, NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { a = NULL; }
            k = a && readable(a, 8) && a[1] == 0x40; how = 1;
        }
        if (part_hash(part) != lastH) { lastH = part_hash(part); clog("vinyl list: row part %08lx -> %s (%s)", lastH, k ? "picture" : "normal", how ? "full-car attribute" : "learned"); }
        return k;
    }
    return g_vpicEdit;
}
static int browser_slot(void)
{
    BYTE *it;
    if (!g_browser || !readable(g_browser, 0x60)) return -1;
    it = *(BYTE **)(g_browser + 0x58);
    if (!it || !readable(it, 0x10)) return -1;
    return *(int *)(it + 0xc);
}
static int is_ice_pk(const char *pk); static int ice_purge(void); static int rows_screen(const char *pk);
/* nav bar layout 0x54e6e0 (thiscall flags, package name; ret 8): on the decal list the game's "Decal Color" button
   (0x800000) is left out, so the bar matches the paint screens (Back | Custom color / Install Decal) */
static void *g_navLayTramp;
static void __cdecl nav_pre(DWORD *fl, const char *pk)
{
    __try {
        if (g_decalOn && readable(pk, 8) && strstr(pk, "Decal") && (*fl & 0x800021) == 0x800021) *fl &= ~0x800000u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
__declspec(naked) static void h_navlayout(void)
{
    __asm {
        mov eax, [esp + 8]
        push ecx
        push eax
        lea eax, [esp + 0xc]
        push eax
        call nav_pre
        add esp, 8
        pop ecx
        jmp dword ptr [g_navLayTramp]
    }
}
static DWORD shop_nav(BYTE *nav)
{
    char *pk, *q; DWORD fl; size_t n; static char last[64]; static DWORD lastFl;
    if (!readable(nav, 0x70)) return 0;
    pk = *(char **)(nav + 0x58); fl = *(DWORD *)(nav + 0x64);
    if (!readable(pk, 8)) return 0;
    n = strnlen(pk, 60);
    if (strncmp(last, pk, 63) || fl != lastFl) {
        if (strncmp(last, pk, 63)) {          /* E3: the parts browser lists vinyls when it is opened from Choose Vinyl Layer */
            int vb = strstr(pk, "GenericParts") ? (strstr(last, "ChooseVinylLayer") ? 1 : strstr(last, "GenericParts") ? g_vinylBrowse : 0) : 0;
            if (vb != g_vinylBrowse) {
                if (!vb) pic_install_watch();                 /* (an install right before leaving) */
                g_vinylBrowse = vb; clog("vinyl shop: parts browser %s", vb ? "lists vinyls" : "left / other parts");
                if (vb) pic_browse_enter(); else InterlockedIncrement(&g_ver);   /* back to each picture's own colour */
            }
        }
        strncpy(last, pk, 63); lastFl = fl; clog("shop screen %s, nav buttons %08lx", pk, fl); }
    if (is_ice_pk(pk)) return (fl & 1) && (fl & 0x08) && g_kindOn[K_PURGE] && ice_purge() ? 0x08 : 0;   /* ICE list on the nitrous purge */
    if (strstr(pk, "Decal")) return g_decalOn && (fl & 1) && (fl & 0x20) ? 0x20 : 0;   /* decal list (nav 00800421): like the paint screens - Install Decal spot = Custom color, Quit spot = Install Decal (D2) */
    for (q = strstr(g_shopPk, pk); q; q = strstr(q + 1, pk))
        if ((q == g_shopPk || q[-1] == ',') && (q[n] == ',' || !q[n])) break;
    if (!q || !(fl & 1)) return 0;
    if (strstr(pk, "Neon") && !g_neonUnlocked) return 0;                    /* neon screens: locked until the career is won */
    if (strstr(pk, "GenericParts")) {          /* parts browser: purge, or a picture vinyl (its tint colour) */
        int bs = browser_slot();
        if (!((bs == 0xa2 && g_kindOn[K_PURGE]) || ((fl & 0x10) && vpic_now() && g_vTint))) return 0;   /* 0x10 = Install Vinyl: the vinyl browser */
    }
    return fl & 0x80 ? 0x80 : fl & 0x08 ? 0x08 : fl & 0x10 ? 0x10 : 0;
}
static DWORD __cdecl nav_label(BYTE *nav, DWORD flag)
{
    DWORD inst;
    __try {
        g_anyNav = nav;
        if (!g_enabled || !(inst = shop_nav(nav))) return 0;
        g_nav = nav;
        if (rows_screen(*(char **)(nav + 0x58))) return 0;                /* CUSTOM row in the list: the game's own buttons */
        if (flag == inst) return g_shopMode ? H_CC_PRESET : H_CC_BTN;
        if (flag == 1) return o_navlabel(nav, NULL, inst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
static BYTE *g_paintScr; static DWORD g_paintScrT;
static int screen_slot(void)                  /* slot id of the paint category open in UI_Paint, -1 = unknown */
{
    BYTE *cat; int slot;
    if (!g_nav || shop_nav(g_nav) != 0x80) return -1;
    /* no time-out: the paint screen object stays valid while we are in the shop (cleared when the shop is left). The old
       3 s time-out made the custom colour come back ~3 s after a preset was clicked with the mouse (arrows kept it fresh) */
    if (!g_paintScr || !readable(g_paintScr, 0x60)) return -1;
    cat = *(BYTE **)(g_paintScr + 0x58);
    if (!cat || !readable(cat, 0x4c)) return -1;
    slot = *(int *)(cat + 0x48);
    return slot >= 0 && slot < 0xaa ? slot : -1;
}
/* the slot the current shop screen edits: paint screen -> its category, neon list -> the neon kind last browsed */
static int nav_is_hud(void);
static int shop_slot_now(void)
{
    DWORD inst;
    if (!g_nav || !(inst = shop_nav(g_nav))) return -1;
    if (strstr(*(char **)(g_nav + 0x58), "Decal")) return -1;
    if (is_ice_pk(*(char **)(g_nav + 0x58))) return 0xa2;
    if (inst == 0x80) return screen_slot();
    if (browser_slot() == 0xa2 && strstr(*(char **)(g_nav + 0x58), "GenericParts")) return 0xa2;
    if (inst == 0x08 && nav_is_hud()) return g_shopSlot >= 0 && SLOTS[g_shopSlot].kind == K_HUD ? SLOTS[g_shopSlot].slot : 0xa4;
    if (inst == 0x08) return g_shopSlot >= 0 && SLOTS[g_shopSlot].kind == K_NEON ? SLOTS[g_shopSlot].slot : 0x9a;
    return -1;
}
static int preset_view(int slot)
{
    int ss;
    if (*(int *)FLOW != 3 || !g_inShop || g_shopMode) return 0;
    ss = shop_slot_now();
    return ss >= 0 && ((slot & 0xFF) == ss);
}
static void preset_installed(void)            /* "Install" while the presets are shown: the player chose a preset */
{
    int ss = shop_slot_now(); BYTE *r = pick_ride(); const char *car = r ? car_name(r) : NULL; Entry *e; int gone = 0;
    if (ss < 0 || !car) return;
    EnterCriticalSection(&g_cs);
    if ((e = find_r(car, r, ss))) { *e = g_e[--g_ne]; gone++; }
    if (shine_of(ss) && (e = find_e(car, shine_of(ss)))) { *e = g_e[--g_ne]; gone++; }
    if (ss == 0xa2 && (e = find_e(car, PURGE_HOOD))) { *e = g_e[--g_ne]; gone++; }
    if (ss == 0xa2 && (e = find_e(car, PURGE_WHEEL))) { *e = g_e[--g_ne]; gone++; }
    if (ss == 0xa2 && (e = find_e(car, PURGE_HOOD2))) { *e = g_e[--g_ne]; gone++; }
    if (ss == 0xa2 && (e = find_e(car, PURGE_WHEEL2))) { *e = g_e[--g_ne]; gone++; }
    LeaveCriticalSection(&g_cs);
    if (gone) { InterlockedIncrement(&g_ver); g_dirty = 1; g_dirtyAt = GetTickCount(); clog("shop: preset installed on slot %02x - custom colour removed", ss); }
}
static int nav_is_hud(void)
{
    char *pk = g_nav && readable(g_nav, 0x70) ? *(char **)(g_nav + 0x58) : NULL;
    return pk && readable(pk, 8) && strstr(pk, "HUD") != NULL;
}
static int default_slot(DWORD inst)
{
    if (g_nav && readable(*(char **)(g_nav + 0x58), 8) && strstr(*(char **)(g_nav + 0x58), "Decal")) {      /* decal place being edited */
        int i2, k2;
        { int pl = -1, sl2 = g_decalTex;    /* (the decal screen's +0x4c is a list object, not the slot - seen live 17.11 13:0x) */
          if (sl2 >= 0x61 && sl2 <= 0x98 && slot_ok(g_pRide, i2 = slot_index(0x200 | sl2)) && SLOTS[i2].slot == (0x200 | sl2)) { clog("decal shop: picker on decal slot %02x (screen)", sl2); return i2; }
          if (pl >= 0) { g_decalPlace = pl; clog("decal shop: picker on decal place %d (screen)", pl); } }
        if (g_decalPlace >= 0) for (k2 = 0; k2 < 8; k2++) { int sl3 = 0x200 | (0x61 + g_decalPlace * 8 + k2); if (SLOTS[i2 = slot_index(sl3)].slot == sl3 && slot_ok(g_pRide, i2)) return i2; }
        for (k2 = 0x61; k2 <= 0x98; k2++) if (SLOTS[i2 = slot_index(0x200 | k2)].slot == (0x200 | k2) && slot_ok(g_pRide, i2)) return i2;
    }
    { int bs = browser_slot(); if (vpic_now() && g_nav && (*(DWORD *)(g_nav + 0x64) & 0x10) && readable(*(char **)(g_nav + 0x58), 8) && strstr(*(char **)(g_nav + 0x58), "GenericParts"))
        return slot_index(0x240 + (g_shownPicL >= 0 ? g_shownPicL : bs >= 0x40 && bs <= 0x43 ? bs - 0x40 : 0)); }   /* picture vinyl tint: its layer's slot */
    if (g_nav && readable(*(char **)(g_nav + 0x58), 8) && is_ice_pk(*(char **)(g_nav + 0x58)) && ice_purge()) return slot_index(PURGE_HOOD);
    if (browser_slot() == 0xa2 && g_nav && readable(*(char **)(g_nav + 0x58), 8) && strstr(*(char **)(g_nav + 0x58), "GenericParts")) return slot_index(PURGE_HOOD);
    int i, want = inst == 0x08 ? (nav_is_hud() ? K_HUD : K_NEON) : K_PAINT, ss = inst == 0x80 ? screen_slot() : -1;
    if (ss >= 0) {
        for (i = 0; i < NSLOT; i++) if (SLOTS[i].slot == ss && slot_ok(g_pRide, i)) { clog("shop: paint category on screen = %s", SLOTS[i].name); return i; }
        clog("shop: paint category slot %02x on screen has no custom colour slot", ss);
    }
    if (g_shopSlot >= 0 && SLOTS[g_shopSlot].kind == want && slot_ok(g_pRide, g_shopSlot)) return g_shopSlot;
    for (i = 0; i < NSLOT; i++) if (SLOTS[i].kind == want && slot_ok(g_pRide, i)) return i;
    return 0;
}
static DWORD g_openT, g_clickEatT; static int g_trkSlot = -1;
static void shop_toggle(BYTE *node, DWORD inst)
{
    const char *c;
    InterlockedIncrement(&g_ver);
    InterlockedIncrement(&g_purgeVer);        /* custom <-> game colours: recolour purge clouds already in the air right away */
    g_delMode = 0;
    if (g_shopMode) { g_shopMode = 0; g_open = 0; clog("shop: preset colours"); }
    else {
        g_pRide = pick_ride();
        if (!g_pRide || !(c = car_name(g_pRide))) { clog("shop: no car found"); return; }
        strncpy(g_pCar, c, 31); g_pCar[31] = 0;
        g_sel = default_slot(inst);
        load_sel();
        if (!sel_custom()) {   /* E1: no own colour yet -> the last picked colour goes on at once (never black / no-effect white) */
            int sl = SLOTS[g_sel].slot, colourless = (sl >= 0x234 && sl <= 0x243) || (sl >= 0x261 && sl <= 0x298);
            DWORD c = g_lastPickOk ? g_lastPick : 0xFFFFFFFF; int k;
            if (c == 0xFFFFFFFF && colourless) { for (k = 0; k < NPRE && !g_pre[k].used; k++); c = k < NPRE ? g_pre[k].base : 0xE01818; }
            if (c != 0xFFFFFFFF) rgb_hsv(c, &g_h, &g_s, &g_v);
            set_custom(1);
            clog("shop: %s takes the picker colour %06lX at once", SLOTS[g_sel].name, hsv_rgb(g_h, g_s, g_v));
        }
        g_shopMode = 1; g_open = 1; g_mDown = 0; g_mDrag = 0; g_openT = GetTickCount(); g_trkSlot = -1;
        clog("shop: custom colour picker for %s, slot %s (ride %p)", g_pCar, SLOTS[g_sel].name, g_pRide);
    }
    if (node && readable(node, 0x20) && *(void **)(node + 0x10)) SetLabel(*(void **)(node + 0x10), g_shopMode ? H_CC_PRESET : H_CC_BTN);
}
static DWORD __cdecl nav_click(BYTE *nav, BYTE *node)
{
    DWORD flag = *(DWORD *)(node + 0x1c), inst;
    __try {
        if (!g_enabled || !(inst = shop_nav(nav))) return flag;
        g_nav = nav;
        if (rows_screen(*(char **)(nav + 0x58))) {                        /* CUSTOM row in the list: game buttons */
            if (flag == inst) { if (g_nlFocus) clog("custom row: CUSTOM installed"); else preset_installed(); }
            return flag;
        }
        if (flag == 1) {                     /* bottom button (was Quit Game) = install; the Q key keeps meaning "quit game?" */
            if (GetAsyncKeyState('Q') & 0x8000) { clog("shop: Q pressed - quit prompt"); return 1; }
            if (!g_shopMode && !(*(DWORD *)(nav + 0x64) & 0x10) && !strstr(*(char **)(nav + 0x58), "Decal")) preset_installed();
            return inst;
        }
        if (flag == inst) { g_clickEatT = GetTickCount(); shop_toggle(node, inst); return 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { clog("shop click: exception"); }
    return flag;
}
/* paint type changed (gloss / metallic / pearl = another part) while the picker is open: the custom colour (and the body
   shine) move to the new part, so only the finish changes */
static void move_custom(int slot, DWORD oldh, DWORD newh)
{
    Entry *e; int moved = 0;
    if (!oldh || !newh || oldh == newh || is_vinyl_slot(slot)) return;
    EnterCriticalSection(&g_cs);
    e = find_e(g_pCar, slot);
    if (e && e->part == oldh) { e->part = newh; moved = 1; }
    if (shine_of(slot) && (e = find_e(g_pCar, shine_of(slot))) && e->part == oldh) { e->part = newh; moved = 1; }
    LeaveCriticalSection(&g_cs);
    if (moved) { InterlockedIncrement(&g_ver); g_dirty = 1; g_dirtyAt = GetTickCount(); clog("shop: custom colour kept on the new paint type (part %08lx -> %08lx)", oldh, newh); }
}
static int __cdecl note_setpart(BYTE *ride, int slot, BYTE *part)
{
    int i;
    if (*(int *)FLOW == 3) { if (slot >= 0x61 && slot <= 0x98) { g_decalPlace = (slot - 0x61) / 8; g_decalTex = slot; } else if (slot >= 0x34 && slot <= 0x3a) g_decalPlace = slot - 0x34; }
    if (slot < 0 || slot > 0xa9 || !readable(ride, 0x590 + 170 * 4) || part_of(ride, slot) == part) return 0;
    /* right after the picker opened the shop may jump the shown car to one of its own presets: keep the custom colour's part */
    if (g_open && g_shopMode && GetTickCount() - g_openT < 600 && slot == (SLOTS[g_sel].slot & 0xFF) && *(int *)FLOW == 3 && ride != (BYTE *)FE_RIDE
        && (ride == g_pRide || mine(ride))) {
        clog("shop: kept the custom colour's paint part on slot %02x (blocked the switch to %08lx)", slot, part_hash(part));
        return 1;
    }
    if (g_open && g_shopMode && part && (ride == (BYTE *)FE_RIDE || ride == g_pRide)) move_custom(slot, part_hash(part_of(ride, slot)), part_hash(part));
    if (ride != (BYTE *)FE_RIDE && !(*(int *)FLOW == 3 && ride == g_pRide) && !(*(int *)FLOW == 3 && mine(ride))) return 0;
    for (i = 0; i < NSLOT; i++) if (SLOTS[i].slot == slot) { g_shopSlot = i; if (g_shopMode && SLOTS[g_sel].kind == SLOTS[i].kind) g_sel = i; break; }
    return 0;
}
/* the same, caught per frame (the shop may swap the part without SetPart): watch the picker slot's part on the shown car */
static DWORD g_trkHash; static BYTE *g_trkRide;
static void track_type(void)
{
    int slot; DWORD h;
    if (!g_open || !g_shopMode || !g_pRide || !readable(g_pRide, 0x590 + 170 * 4)) { g_trkSlot = -1; return; }
    slot = SLOTS[g_sel].slot & 0xFF;
    h = part_hash(part_of(g_pRide, slot));
    if (slot == g_trkSlot && g_pRide == g_trkRide && h != g_trkHash) move_custom(slot, g_trkHash, h);
    g_trkSlot = slot; g_trkHash = h; g_trkRide = g_pRide;
}
__declspec(naked) static void h_navlabel(void)
{
    __asm {
        mov eax, [esp + 4]
        push eax
        push ecx
        call nav_label
        add esp, 8
        test eax, eax
        jz orig
        ret 4
    orig:
        jmp dword ptr [o_navlabel]
    }
}
__declspec(naked) static void h_navclick(void)
{
    __asm {
        pushad
        push esi
        push edi
        call nav_click
        add esp, 8
        mov g_clickRes, eax
        popad
        mov eax, g_clickRes
        test eax, eax
        jz consumed
        test al, 2
        jmp dword ptr [g_clickBack]
    consumed:
        mov ecx, ds:[0x8651ac]
        mov [edi + 0x68], ecx
        pop esi
        pop ebx
        pop edi
        ret
    }
}
__declspec(naked) static void h_lang(void)
{
    __asm {
        cmp edx, H_CC_BTN
        je b1
        cmp edx, H_CC_PRESET
        je b2
        cmp edx, g_nlLangHash             /* the name of the part under CUSTOM: "CUSTOM" while that row is selected */
        je b3
    orig:
        jmp dword ptr [o_lang]
    b3:
        push ecx
        push edx
        call nl_lang_text
        pop edx
        pop ecx
        test eax, eax
        jz orig
        ret
    b1:
        lea eax, S_CC_BTN
        ret
    b2:
        lea eax, S_CC_PRESET
        ret
    }
}
__declspec(naked) static void h_setpart(void)
{
    __asm {
        pushad
        mov eax, [esp + 0x28]
        push eax
        mov eax, [esp + 0x28]
        push eax
        push ecx
        call note_setpart
        add esp, 12
        mov [esp + 0x1c], eax
        popad
        test eax, eax
        jnz skip
        jmp dword ptr [o_setpart]
    skip:
        ret 8
    }
}
/* Where the front end's 640x480 space lands on screen. With NFSUnderground2.WidescreenFix (FixHUD=1) the menus are a centred
   4:3 area; the stock game (or the fix with FixHUD=0) stretches them over the whole window. ini MenuLayout: 0 = detect,
   1 = centred, 2 = stretched. sx / sy map FE positions and spans, s is the uniform size for round dots, text and buttons
   (never stretched, so they keep their shape on wide screens). */
static int g_menuLayout, g_wsFix = -1;
static int ws_centred(void)
{
    if (g_menuLayout == 1) return 1;
    if (g_menuLayout == 2) return 0;
    if (g_wsFix < 0) {
        HMODULE m = GetModuleHandleA("NFSUnderground2.WidescreenFix.asi"); char ip[MAX_PATH], *e;
        g_wsFix = 0;
        if (m && GetModuleFileNameA(m, ip, MAX_PATH) && (e = strrchr(ip, '.')) != NULL) {
            strcpy(e, ".ini");
            g_wsFix = GetPrivateProfileIntA("MAIN", "FixHUD", 1, ip) ? 1 : 0;
        }
        clog("menu layout: %s", m ? (g_wsFix ? "WidescreenFix found, menus centred 4:3" : "WidescreenFix found with FixHUD=0, menus stretched")
                                  : "no WidescreenFix, menus stretched to the window");
    }
    return g_wsFix;
}
static void fe_map(int W, int H, float *s, float *ox, float *oy, float *sx, float *sy)
{
    if (!ws_centred()) { *sx = W / 640.f; *sy = H / 480.f; *s = *sx < *sy ? *sx : *sy; *ox = *oy = 0; return; }
    if ((float)W / H >= 4.f / 3.f) { *s = H / 480.f; *ox = (W - 640.f * *s) / 2; *oy = 0; }
    else { *s = W / 640.f; *ox = 0; *oy = (H - 480.f * *s) / 2; }
    *sx = *sy = *s;
}
/* the shop's colour-dot box, in the front end's 640x480 space */
#define BOX_L 334.f
#define BOX_T 380.f
#define BOX_R 619.f
#define BOX_B 428.f
/* E6: preset row for picture vinyls - "Default" (the picture as it is) + the game's own vinyl colours. The palette is read
   from the parts table (list 0x8a2b68: pack +0x34 parts of 0xe bytes, +0x38 count; part +4 = type, vinyl colours = type
   0x31; colour = its RED / GREEN / BLUE attributes): white, black, then the colours by hue, sampled to fit one row. */
static void vinyl_palette(void)
{
    DWORD all[160]; float hh[160]; int na = 0, i, j, nc = 0, idx[160];
    if (g_nvpal >= 0) return;
    g_nvpal = 0;
    __try {
        BYTE *head = (BYTE *)0x8a2b68, *pk; int packs = 0;
        for (pk = *(BYTE **)head; pk && pk != head && readable(pk, 0x3c) && packs < 64; pk = *(BYTE **)pk, packs++) {
            BYTE *arr = *(BYTE **)(pk + 0x34); int n = *(int *)(pk + 0x38);
            if (!arr || n <= 0 || n > 200000 || !readable(arr, (size_t)n * 0xe)) continue;
            for (i = 0; i < n && na < 160; i++) {
                BYTE *pt = arr + i * 0xe; DWORD *a, r, g, b, c; int k;
                if (pt[4] != 0x31) continue;
                if (!(a = o_attr(pt, NULL, H_RED, NULL))) continue;
                r = a[1] & 255; a = o_attr(pt, NULL, H_GREEN, NULL); g = a ? a[1] & 255 : 0; a = o_attr(pt, NULL, H_BLUE, NULL); b = a ? a[1] & 255 : 0;
                c = (r << 16) | (g << 8) | b;
                for (k = 0; k < na && all[k] != c; k++);
                if (k == na) all[na++] = c;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { clog("vinyl palette: exception 0x%08lx", GetExceptionCode()); }
    if (!na) {                                            /* not found: a fixed set in the game's style */
        static const DWORD F[NVPAL] = { 0xFFFFFF, 0x101010, 0xD01010, 0xF07010, 0xF0D010, 0x60D020, 0x10A060, 0x10B0E0, 0x1040D0, 0x7020C0, 0xE020A0 };
        memcpy(g_vpal, F, sizeof F); g_nvpal = NVPAL; clog("vinyl palette: game colours not found - fixed set"); return;
    }
    {   int wi = -1, bi = -1; float wv = -1, bv = 2;
        for (i = 0; i < na; i++) {
            float h, sa, v; rgb_hsv(all[i], &h, &sa, &v);
            if (sa < 0.18f) { if (v > wv) { wv = v; wi = i; } if (v < bv) { bv = v; bi = i; } continue; }
            hh[nc] = h; idx[nc++] = i;
        }
        for (i = 1; i < nc; i++) for (j = i; j > 0 && hh[j - 1] > hh[j]; j--) { float t = hh[j]; int u = idx[j]; hh[j] = hh[j - 1]; idx[j] = idx[j - 1]; hh[j - 1] = t; idx[j - 1] = u; }
        if (wi >= 0) g_vpal[g_nvpal++] = all[wi];
        if (bi >= 0 && bi != wi) g_vpal[g_nvpal++] = all[bi];
        {   int want = NVPAL - g_nvpal, last = -1;
            for (j = 0; j < want && nc; j++) {
                int k = want > 1 ? (int)((float)j * (nc - 1) / (want - 1) + 0.5f) : 0;
                if (k == last) continue; last = k; g_vpal[g_nvpal++] = all[idx[k]];
            }
        }
    }
    {   char b2[160]; int o = 0; for (i = 0; i < g_nvpal && o < 150; i++) o += sprintf(b2 + o, " %06lX", g_vpal[i]);
        clog("vinyl palette: %d game vinyl colours, row:%s", na, b2); }
}
static void draw_shop_picker(IDirect3DDevice9 *d, int W, int H)
{
    float s, ox, oy, sx, sy, bx, by, bw, bh, ts; char buf[64]; DWORD cur = hsv_rgb(g_h, g_s, g_v), hue = argb(hsv_rgb(g_h, 1, 1), 255);
    int on = sel_custom();
    DWORD shown = on ? cur : game_rgb(slot_ride(g_sel), SLOTS[g_sel].slot);
    if (!on && !g_mDown && cur != shown) {      /* E7: the square, the hue bar and the swatch always show the same colour */
        rgb_hsv(shown, &g_h, &g_s, &g_v); cur = hsv_rgb(g_h, g_s, g_v); hue = argb(hsv_rgb(g_h, 1, 1), 255);
    }
    fe_map(W, H, &s, &ox, &oy, &sx, &sy);
    g_W = W; g_H = H;
    bx = ox + BOX_L * sx; by = oy + BOX_T * sy; bw = (BOX_R - BOX_L) * sx; bh = (BOX_B - BOX_T) * sy;
    g_boxL = (int)(bx / W * 10000); g_boxR = (int)((bx + bw) / W * 10000); g_boxT = (int)((by - 17 * s) / H * 10000); g_boxB = (int)((by + bh) / H * 10000);
    {   /* my colours row */
        int i; float ry = by - 17 * s, sw = 12 * s, x = bx + 52 * s;
        quad(d, bx - 2 * s, ry - 1 * s, bw + 4 * s, 16 * s, 0xFF141418);
        if (g_delMode && GetAsyncKeyState(VK_ESCAPE) & 0x8000) g_delMode = 0;          /* Esc cancels Delete mode */
        text(d, g_delMode ? "Delete:" : "My colours", bx + 3 * s, ry + 1.5f * s, 8.5f * s / g_fontH, g_delMode ? 0xFFFF6A5A : GAME_GREEN);
        for (i = 0; i < NPRE; i++, x += sw + 3 * s) {
            float mx = g_mx / 10000.f * W, my = g_my / 10000.f * H;
            int hov = g_mx >= 0 && g_pre[i].used && in_r(g_preR[i], mx, my);
            g_preR[i][0] = x; g_preR[i][1] = ry + 1 * s; g_preR[i][2] = sw; g_preR[i][3] = sw;
            if (g_pre[i].used) {
                DWORD a = argb(g_pre[i].base, 255), b2 = argb(g_pre[i].hasShine ? g_pre[i].shine : g_pre[i].base, 255);
                quad4(d, x, ry + 1 * s, sw, sw, a, b2, a, b2);
                frame(d, x, ry + 1 * s, sw, sw, 0.6f * s, g_delMode ? 0xFFFF5050 : 0xFF8A8A96);
            } else frame(d, x, ry + 1 * s, sw, sw, 0.6f * s, 0xFF3A3A42);
            if (hov) frame(d, x - 1 * s, ry, sw + 2 * s, sw + 2 * s, 1.f * s, g_delMode ? 0xFFFF5050 : GAME_GREEN);   /* the one a click hits */
        }
        g_saveR[0] = bx + bw - 36 * s; g_saveR[1] = ry + 1 * s; g_saveR[2] = 36 * s; g_saveR[3] = sw;
        button(d, g_saveR[0], g_saveR[1], g_saveR[2], g_saveR[3], s, 0);
        text(d, "Save", g_saveR[0] + (g_saveR[2] - text_w("Save", 8.5f * s / g_fontH)) / 2, g_saveR[1] + 1.f * s, 8.5f * s / g_fontH, 0xFFFFFFFF);
        g_delR[2] = 38 * s; g_delR[3] = sw; g_delR[0] = g_saveR[0] - 3 * s - g_delR[2]; g_delR[1] = g_saveR[1];   /* Delete, left of Save */
        button(d, g_delR[0], g_delR[1], g_delR[2], g_delR[3], s, g_delMode);
        text(d, "Delete", g_delR[0] + (g_delR[2] - text_w("Delete", 8.5f * s / g_fontH)) / 2, g_delR[1] + 1.f * s, 8.5f * s / g_fontH, g_delMode ? 0xFFFF6A5A : 0xFFFFFFFF);
    }
    quad(d, bx - 2 * s, by - 2 * s, bw + 4 * s, bh + 4 * s, 0xFF141418);
    frame(d, bx, by, bw, bh, 1.f * s, 0xFF4A4A52);
    /* SV square */
    g_bsz = bh - 6 * s; g_bx = bx + 3 * s; g_by = by + 3 * s;
    quad4(d, g_bx, g_by, g_bsz, g_bsz, 0xFFFFFFFF, hue, 0xFFFFFFFF, hue);
    quad4(d, g_bx, g_by, g_bsz, g_bsz, 0x00000000, 0x00000000, 0xFF000000, 0xFF000000);
    frame(d, g_bx + g_s * g_bsz - 2 * s, g_by + (1 - g_v) * g_bsz - 2 * s, 4 * s, 4 * s, 0.7f * s, g_v > 0.5f ? 0xFF000000 : 0xFFFFFFFF);
    /* hue bar */
    g_hx = g_bx + g_bsz + 6 * s; g_hy = by + 4 * s; g_hw = bw - (g_hx - bx) - bh - 4 * s; g_hh = 14 * s; g_hHoriz = 1;
    hue_bar(d, g_hx, g_hy, g_hw, g_hh, 0);
    frame(d, g_hx + g_h / 360.f * g_hw - 1.5f * s, g_hy - 1.5f * s, 3 * s, g_hh + 3 * s, 0.8f * s, 0xFFFFFFFF);
    /* the colour now (body paint / shine: a base -> shine gradient) */
    g_tabBase = SLOTS[g_sel].slot & 0xFF;
    g_tabsOn = (SLOTS[g_sel].kind == K_PAINT || SLOTS[g_sel].kind == K_SHINE) && shine_of(g_tabBase) && g_kindOn[K_SHINE] ? 1
             : SLOTS[g_sel].kind == K_PURGE ? 2 : 0;
    if (g_tabsOn == 1) {
        int bi = slot_index(g_tabBase), si = slot_index(shine_of(g_tabBase)); DWORD bc, sc2; Entry *e; BYTE *r = slot_ride(bi);
        EnterCriticalSection(&g_cs);
        e = find_e(g_pCar, g_tabBase); bc = e && 0 != part_hash(part_of(r, g_tabBase)) ? e->rgb : game_rgb(r, g_tabBase);
        e = find_e(g_pCar, shine_of(g_tabBase)); sc2 = e && 0 != part_hash(part_of(r, g_tabBase)) ? e->rgb : bc;
        LeaveCriticalSection(&g_cs);
        if (g_sel == bi && on) bc = cur;
        if (g_sel == si && on) sc2 = cur;
        quad4(d, bx + bw - bh + 3 * s, g_by, g_bsz, g_bsz, argb(bc, 255), argb(sc2, 255), argb(bc, 255), argb(sc2, 255));
    } else
        quad(d, bx + bw - bh + 3 * s, g_by, g_bsz, g_bsz, argb(shown, 255));
    frame(d, bx + bw - bh + 3 * s, g_by, g_bsz, g_bsz, 0.8f * s, 0xFF4A4A52);
    ts = 10.f * s / g_fontH;
    sprintf(buf, "%s   #%06X", SLOTS[g_sel].label, shown);
    text(d, buf, g_hx, g_hy + g_hh + 2.5f * s, ts, GAME_GREEN);
    g_glR[2] = 0;
    if (g_tabsOn == 1 && g_tabBase == 0x3f) {   /* body: gloss slider instead of the hint (reflection strength of the paint) */
        BYTE *rr = slot_ride(g_sel); DWORD gl; Entry *ge; DWORD *ga; int has;
        EnterCriticalSection(&g_cs); ge = find_e(g_pCar, GLOSS_SLOT); has = ge != NULL; gl = has ? ge->rgb & 255 : 0; LeaveCriticalSection(&g_cs);
        if (!has) { BYTE *pp = part_of(rr, 0x3f); ga = pp ? o_attr(pp, NULL, H_GLOSS, NULL) : NULL; gl = ga ? ga[1] & 255 : 128; }
        text(d, "Gloss", g_hx, g_hy + g_hh + 13.5f * s, 7.5f * s / g_fontH, GAME_GREEN);
        g_glR[0] = g_hx + 24 * s; g_glR[1] = g_hy + g_hh + 16.5f * s; g_glR[2] = g_hw - 24 * s - 86 * s; g_glR[3] = 4 * s;
        quad4(d, g_glR[0], g_glR[1], g_glR[2], g_glR[3], 0xFF2A2A30, 0xFFE0E0E8, 0xFF2A2A30, 0xFFE0E0E8);
        frame(d, g_glR[0] + gl / 255.f * g_glR[2] - 1.5f * s, g_glR[1] - 2 * s, 3 * s, g_glR[3] + 4 * s, 0.8f * s, has ? 0xFFFFFFFF : 0xFF8A8A96);
    } else if (g_tabsOn == 0 && IS_PIC(SLOTS[g_sel].slot)) {
        float y0 = g_hy + g_hh + 12.f * s, sw = 10 * s, x, ls = 7.5f * s / g_fontH, mx = g_mx / 10000.f * W, my = g_my / 10000.f * H; int k;
        vinyl_palette(); g_vpOn = 1;
        g_vpDef[0] = g_hx; g_vpDef[1] = y0; g_vpDef[2] = 30 * s; g_vpDef[3] = sw;
        button(d, g_vpDef[0], g_vpDef[1], g_vpDef[2], g_vpDef[3], s, !on);
        text(d, "Default", g_vpDef[0] + (g_vpDef[2] - text_w("Default", ls)) / 2, y0 + 1.f * s, ls, !on ? GAME_GREEN : 0xFFFFFFFF);
        if (g_mx >= 0 && in_r(g_vpDef, mx, my)) frame(d, g_vpDef[0] - 1 * s, y0 - 1 * s, g_vpDef[2] + 2 * s, sw + 2 * s, 1.f * s, GAME_GREEN);
        x = g_vpDef[0] + g_vpDef[2] + 4 * s;
        {   float gap = g_nvpal > 1 ? (g_hx + g_hw - x - g_nvpal * sw) / (g_nvpal - 1) : 0;    /* the row ends with the hue bar */
            if (gap < 1.5f * s) gap = 1.5f * s; if (gap > 6 * s) gap = 6 * s;
        for (k = 0; k < g_nvpal; k++, x += sw + gap) {
            int sel = on && cur == g_vpal[k], hov = g_mx >= 0 && in_r(g_vpR[k], mx, my);
            g_vpR[k][0] = x; g_vpR[k][1] = y0; g_vpR[k][2] = sw; g_vpR[k][3] = sw;
            if (x + sw > g_hx + g_hw) { g_vpR[k][2] = 0; continue; }
            quad(d, x, y0, sw, sw, argb(g_vpal[k], 255));
            frame(d, x, y0, sw, sw, 0.6f * s, 0xFF8A8A96);
            if (sel || hov) frame(d, x - 1 * s, y0 - 1 * s, sw + 2 * s, sw + 2 * s, 1.f * s, GAME_GREEN);
        } }
    } else
        { g_vpOn = 0; if (g_tabsOn == 0) text(d, "drag to pick   arrows: colour / brightness   PgUp/PgDn: part", g_hx, g_hy + g_hh + (sat_slot_ok(SLOTS[g_sel].slot) ? 13.f : 15.f) * s, 7.5f * s / g_fontH, 0xFF8A8A96); }
    g_shR[2] = 0;
    if (g_tabsOn == 1) {                    /* Shine amount: how much of the shine colour (0 = the base colour only) */
        BYTE *rr = slot_ride(g_sel); Entry *ae; DWORD am, bc2, sc3; int ah; float sx0 = g_hx + g_hw - 74 * s;
        EnterCriticalSection(&g_cs);
        ae = find_e(g_pCar, 0x300 | g_tabBase); ah = ae && 0 != part_hash(part_of(rr, g_tabBase)); am = ah ? ae->rgb & 255 : 128;
        ae = find_e(g_pCar, g_tabBase); bc2 = ae && 0 != part_hash(part_of(rr, g_tabBase)) ? ae->rgb : game_rgb(rr, g_tabBase);
        ae = find_e(g_pCar, shine_of(g_tabBase)); sc3 = ae && 0 != part_hash(part_of(rr, g_tabBase)) ? ae->rgb : bc2;
        LeaveCriticalSection(&g_cs);
        if (SLOTS[g_sel].slot == g_tabBase && sel_custom()) bc2 = hsv_rgb(g_h, g_s, g_v);
        if (SLOTS[g_sel].slot == shine_of(g_tabBase) && sel_custom()) sc3 = hsv_rgb(g_h, g_s, g_v);
        quad(d, sx0 - 5 * s, g_hy + g_hh + 13 * s, 1 * s, 10 * s, 0xFF4A4A52);
        text(d, "Shine", sx0, g_hy + g_hh + 13.5f * s, 7.5f * s / g_fontH, GAME_GREEN);
        g_shR[0] = sx0 + 24 * s; g_shR[1] = g_hy + g_hh + 16.5f * s; g_shR[2] = g_hx + g_hw - g_shR[0] - 2 * s; g_shR[3] = 4 * s;
        quad4(d, g_shR[0], g_shR[1], g_shR[2], g_shR[3], argb(bc2, 255), argb(sc3, 255), argb(bc2, 255), argb(sc3, 255));
        frame(d, g_shR[0] - 0.5f * s, g_shR[1] - 0.5f * s, g_shR[2] + 1 * s, g_shR[3] + 1 * s, 0.5f * s, 0xFF4A4A52);
        quad(d, g_shR[0] + 128.f / 255.f * g_shR[2] - 0.25f * s, g_shR[1] + g_shR[3] + 1 * s, 0.5f * s, 2 * s, 0xFF8A8A96);   /* 100 % mark */
        frame(d, g_shR[0] + am / 255.f * g_shR[2] - 1.5f * s, g_shR[1] - 2 * s, 3 * s, g_shR[3] + 4 * s, 0.8f * s, ah ? 0xFFFFFFFF : 0xFF8A8A96);
    }
    g_saR[2] = 0;
    if (g_tabsOn == 1 || (g_tabsOn == 0 && sat_slot_ok(SLOTS[g_sel].slot))) {   /* v1.1 Saturation (0..200 %, 100 % = as before) */
        BYTE *rr = slot_ride(g_sel); Entry *se; DWORD sv, bcs; int sh2; char pct[16];
        float ly = g_tabsOn == 1 ? 21.5f : 22.5f, by2 = g_tabsOn == 1 ? 24.5f : 25.f;      /* (picture vinyls: under the preset row) */
        EnterCriticalSection(&g_cs);
        se = find_e(g_pCar, 0x500 | g_tabBase); sh2 = se != NULL; sv = sh2 ? se->rgb & 255 : 128;
        if (g_tabsOn == 1) { se = find_e(g_pCar, g_tabBase); bcs = se && 0 != part_hash(part_of(rr, g_tabBase)) ? se->rgb : game_rgb(rr, g_tabBase); }
        else bcs = shown;
        LeaveCriticalSection(&g_cs);
        if (g_tabsOn == 1 && SLOTS[g_sel].slot == g_tabBase && sel_custom()) bcs = hsv_rgb(g_h, g_s, g_v);
        text(d, "Saturation", g_hx, g_hy + g_hh + ly * s, 7.5f * s / g_fontH, GAME_GREEN);
        g_saR[0] = g_hx + 40 * s; g_saR[1] = g_hy + g_hh + by2 * s; g_saR[2] = g_hw - 40 * s - 26 * s; g_saR[3] = 3.5f * s;
        {   DWORD c0 = argb(sat_rgb(bcs, 0.f), 255), c1 = argb(bcs, 255), c2 = argb(sat_rgb(bcs, 2.f), 255); float hw2 = g_saR[2] * 128.f / 255.f;
            quad4(d, g_saR[0], g_saR[1], hw2, g_saR[3], c0, c1, c0, c1);
            quad4(d, g_saR[0] + hw2, g_saR[1], g_saR[2] - hw2, g_saR[3], c1, c2, c1, c2); }
        frame(d, g_saR[0] - 0.5f * s, g_saR[1] - 0.5f * s, g_saR[2] + 1 * s, g_saR[3] + 1 * s, 0.5f * s, 0xFF4A4A52);
        quad(d, g_saR[0] + 128.f / 255.f * g_saR[2] - 0.25f * s, g_saR[1] + g_saR[3] + 1 * s, 0.5f * s, 1.5f * s, 0xFF8A8A96);   /* 100 % mark */
        frame(d, g_saR[0] + sv / 255.f * g_saR[2] - 1.5f * s, g_saR[1] - 2 * s, 3 * s, g_saR[3] + 4 * s, 0.8f * s, sh2 ? 0xFFFFFFFF : 0xFF8A8A96);
        sprintf(pct, "%d %%", (int)(sv * 100 / 128.f + 0.5f));
        text(d, pct, g_saR[0] + g_saR[2] + 4 * s, g_hy + g_hh + ly * s, 7.5f * s / g_fontH, sh2 ? 0xFFFFFFFF : 0xFF8A8A96);
    }
    if (g_tabsOn) {                       /* Base / Shine tabs, right end of the name row */
        int t;
        for (t = 0; t < 2; t++) {
            int act = g_tabsOn == 2 ? (t == 0 ? !purge_wheels() : t == 1 ? purge_wheels() : t == 2 ? !purge_col2() : purge_col2())
                                    : SLOTS[g_sel].slot == (t ? shine_of(g_tabBase) : g_tabBase);
            g_tab[t][2] = 34 * s; g_tab[t][3] = 11 * s; g_tab[t][0] = g_hx + g_hw - (2 - (t & 1)) * 36 * s; g_tab[t][1] = g_hy + g_hh + (t < 2 ? 2 : 15) * s;
            { const char *lb = g_tabsOn == 2 ? (t == 0 ? "Hood" : t == 1 ? "Wheels" : t == 2 ? "Colour 1" : "Colour 2") : (t ? "Shine" : "Base");
              float ls = 8.5f * s / g_fontH;
              button(d, g_tab[t][0], g_tab[t][1], g_tab[t][2], g_tab[t][3], s, act);
              text(d, lb, g_tab[t][0] + (g_tab[t][2] - text_w(lb, ls)) / 2, g_tab[t][1] + 1.f * s, ls, act ? GAME_GREEN : 0xFFFFFFFF); }
        }
    }
    if (SLOTS[g_sel].kind == K_NEON && part_of(g_pRide, SLOTS[g_sel].slot) && !(((BYTE *)part_of(g_pRide, SLOTS[g_sel].slot))[5] >> 5))
        text(d, "pick a neon first", bx + bw - bh - 70 * s, g_hy + g_hh + 3 * s, ts, 0xFFFF9060);
    if (g_ownCursor && g_mx >= 0) {           /* the game's cursor is hidden over the picker: draw one */
        float mx = g_mx / 10000.f * W, my = g_my / 10000.f * H, k = s * 0.9f;
        quad(d, mx - 7 * k, my - 1.5f * k, 14 * k, 3 * k, 0xFF000000); quad(d, mx - 1.5f * k, my - 7 * k, 3 * k, 14 * k, 0xFF000000);
        quad(d, mx - 6 * k, my - 0.5f * k, 12 * k, 1 * k, 0xFFFFFFFF); quad(d, mx - 0.5f * k, my - 6 * k, 1 * k, 12 * k, 0xFFFFFFFF);
    }
}
/* UI_Paint screen messages (0x563410, thiscall msg,a,b,c ret 0x10): while the picker is open the colour dots get none of
   PAD_LEFT/RIGHT/UP/DOWN (move the dot cursor) or the mouse click 0x9803f6e2 / hover-blink 0xb4edeb6d.
   PAD_ACCEPT (Install), PAD_BACK and the rest still reach the shop. */
#define A_PAINTMSG 0x563410
static void *o_paintmsg; static DWORD g_blkLog;
static int __cdecl paint_msg_blocked(DWORD msg, BYTE *scr)
{
    g_paintScr = scr; g_paintScrT = GetTickCount();
    if (msg == 0x406415e3 && g_enabled && g_inShop && !g_shopMode) preset_installed();      /* PAD_ACCEPT = install */
    if (msg == 0x9803f6e2 && GetTickCount() - g_clickEatT < 400) return 1;                   /* the click on our button is not a dot pick */
    if (!g_enabled || !g_open || !g_shopMode) return 0;
    switch (msg) {
    case 0x9120409e: case 0xb5971bf1: case 0x72619778: case 0x911c0a4b:   /* mouse clicks pass: the paint type arrows stay usable,
                                                                            the dots never see the cursor (hidden over the picker) */
        if (GetTickCount() - g_blkLog > 2000) { g_blkLog = GetTickCount(); clog("shop: colour dots blocked (message %08lx)", msg); }
        return 1;
    }
    return 0;
}
__declspec(naked) static void h_paintmsg(void)
{
    __asm {
        push ecx
        push ecx
        push dword ptr [esp + 0xc]
        call paint_msg_blocked
        add esp, 8
        pop ecx
        test eax, eax
        jnz blocked
        jmp dword ptr [o_paintmsg]
    blocked:
        ret 0x10
    }
}
/* K / L turn the car in the front-end shops. The shown cars are in the list at 0x8a30f0 (node +8 render info, +0xb64 shown,
   +0x910 position, +0x920 rotation matrix); body, wheels, neon and shadow all use +0x920. Right before the front-end car
   render (0x630de0) the plugin writes base rotation * yaw; when the game puts a new rotation there it becomes the new base. */
#define A_FECARS 0x630de0
#define FE_CARLIST 0x8a30f0
typedef struct { BYTE *obj; float base[16], last[16]; int on; } Turn;
static Turn g_turn[4]; static float g_yaw; static DWORD g_turnT; static void *o_fecars;
static int fe_car_shown(void)
{
    BYTE *n = *(BYTE **)FE_CARLIST; int k = 0;
    for (; n && n != (BYTE *)FE_CARLIST && k < 8 && readable(n, 0xb70); n = *(BYTE **)n, k++)
        if (*(void **)(n + 8) && *(DWORD *)(n + 0xb64)) return 1;
    return 0;
}
static int turn_active(void)
{
    char *pk;
    if (!g_turnOn || !g_enabled || *(int *)FLOW != 3 || !g_anyNav || !readable(g_anyNav, 0x70) || !*(BYTE *)(g_anyNav + 0x6c)) return 0;
    pk = *(char **)(g_anyNav + 0x58);
    if (!readable(pk, 8) || strstr(pk, "Keyboard") || strstr(pk, "Name") || strstr(pk, "Profile") || strstr(pk, "Main.fng")) return 0;
    return fe_car_shown();
}
static void turn_apply_(void)
{
    BYTE *n = *(BYTE **)FE_CARLIST; int k = 0, i, active = turn_active(); DWORD now = GetTickCount(); float dt, c, sn;
    dt = g_turnT ? (now - g_turnT) / 1000.f : 0; if (dt > 0.1f) dt = 0.1f; g_turnT = now;
    { static int was = -1; if (active != was) { char *pk = g_anyNav && readable(g_anyNav, 0x70) ? *(char **)(g_anyNav + 0x58) : NULL;
        clog("turntable: K / L %s (screen %s)", active ? "active" : "off", pk && readable(pk, 8) ? pk : "?"); was = active; } }
    if (active) {
        /* keys straight from Windows (the game's own keyboard reading did not deliver K / L reliably) */
        int fg = game_front(), k = fg && ((GetAsyncKeyState('K') & 0x8000) || (g_dikHeld[0x25] && g_dikTaken[0x25])),
            l = fg && ((GetAsyncKeyState('L') & 0x8000) || (g_dikHeld[0x26] && g_dikTaken[0x26]));
        float sp = (g_dikHeld[0x2A] || g_dikHeld[0x36] || (fg && (GetAsyncKeyState(VK_SHIFT) & 0x8000))) ? 3.4f : 1.6f;   /* rad/s */
        static int wasK, wasL;
        if ((k && !wasK) || (l && !wasL)) clog("turntable: %s pressed (screen active)", k ? "K" : "L");
        wasK = k; wasL = l;
        if (k) g_yaw += sp * dt;
        if (l) g_yaw -= sp * dt;
        if (g_yaw > 6.2831853f) g_yaw -= 6.2831853f;
        if (g_yaw < -6.2831853f) g_yaw += 6.2831853f;
    } else g_yaw = 0;
    c = cosf(g_yaw); sn = sinf(g_yaw);
    for (; n && n != (BYTE *)FE_CARLIST && k < 4 && readable(n, 0xb70); n = *(BYTE **)n, k++) {
        Turn *t = NULL; float *m = (float *)(n + 0x920);
        for (i = 0; i < 4; i++) if (g_turn[i].obj == n) { t = &g_turn[i]; break; }
        if (!t) {
            if (g_yaw == 0) continue;
            for (i = 0; i < 4; i++) if (!g_turn[i].on) { t = &g_turn[i]; break; }
            if (!t) continue;
            t->obj = n; t->on = 0;
        }
        if (!t->on || memcmp(m, t->last, 64)) { memcpy(t->base, m, 64); t->on = 1; }   /* the game set a new rotation */
        if (g_yaw == 0) { memcpy(m, t->base, 64); t->on = 0; t->obj = NULL; continue; }
        for (i = 0; i < 4; i++) {
            float x = t->base[i], y = t->base[4 + i];
            m[i] = c * x + sn * y; m[4 + i] = -sn * x + c * y;
        }
        memcpy(t->last, m, 64);
    }
}
/* turntable yaw for other plugins (HDToggle turns the shop's turntable model with it): shared memory "Local\NFSU2_TurnYaw",
   { float yaw_rad; DWORD seq; }, written every frame. yaw = the angle the FE car is turned by K / L (0 when no shop / turn is
   active); positive = counter-clockwise seen from above (about +Z: the car's x axis turns towards +y). */
static void publish_yaw(void)
{
    static struct { float yaw; DWORD seq; } *sh; static int tried;
    if (!sh && !tried) {
        HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 8, "Local\\NFSU2_TurnYaw"); tried = 1;
        if (h) sh = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 8);
        clog("turntable yaw shared memory: %s", sh ? "ok" : "FAILED");
    }
    if (sh) { sh->yaw = turn_active() ? g_yaw : 0.f; sh->seq++; }
}
static void __cdecl turn_apply(void)
{
    __try { turn_apply_(); } __except (EXCEPTION_EXECUTE_HANDLER) { static int e; if (e++ < 3) clog("turntable: exception"); }
}
__declspec(naked) static void h_fecars(void)
{
    __asm {
        pushad
        call turn_apply
        popad
        jmp dword ptr [o_fecars]
    }
}
static BYTE *nav_node(BYTE *nav, DWORD flag)
{
    BYTE *first, *n; int k;
    if (!readable(nav, 0x70)) return NULL;
    first = *(BYTE **)(nav + 0x4c);
    for (n = first ? first - 4 : NULL, k = 0; n && k < 16 && readable(n, 0x20); k++) {
        BYTE *nx;
        if (*(DWORD *)(n + 0x1c) == flag) return n;
        nx = *(BYTE **)(n + 4);
        if (!nx || nx == first || nx == nav + 0x4c) break;
        n = nx - 4;
    }
    return NULL;
}
static DWORD g_shopEnterT; static int g_autoDone;
static void auto_open(void)
{
    DWORD inst; int ss; BYTE *r, *node; const char *car; Entry *e; int has = 0;
    if (g_autoDone || g_shopMode || !g_nav || (inst = shop_nav(g_nav)) != 0x80) return;
    if (GetTickCount() - g_shopEnterT < 150 || g_paintScrT < g_shopEnterT) return;   /* wait for this screen's own object */
    g_autoDone = 1;
    if ((ss = screen_slot()) < 0 || !(r = pick_ride()) || !(car = car_name(r))) return;
    EnterCriticalSection(&g_cs);
    /* only when the slot still has the paint part the custom colour was made on (a preset installed over it = open the
       game's own preset list, not the picker) */
    { DWORD ph = part_hash(part_of(r, ss));
      e = find_r(car, r, ss); if (e && ph && (!e->part || e->part == ph || is_vinyl_slot(ss))) has = 1;
      if (shine_of(ss)) { e = find_e(car, shine_of(ss)); if (e && ph && (!e->part || e->part == ph)) has = 1; } }
    LeaveCriticalSection(&g_cs);
    if (!has) return;
    node = nav_node(g_nav, inst);
    clog("shop: slot %02x has a custom colour - opening the picker", ss);
    shop_toggle(node, inst);
}
#define A_NISSTART 0x484510
static void *o_nis; static DWORD H_ENDING;
static void save_unlocks(void)
{
    FILE *f = fopen(g_unlockFile, "w");
    if (!f) { clog("cannot write %s", g_unlockFile); return; }
    fprintf(f, "; NFSU2_CustomColor - rewards. NeonCustom=1 after the final career race was won (ending scene played).\nNeonCustom=%d\n", g_neonUnlocked);
    fclose(f);
}
static void __cdecl nis_started(DWORD hash)
{
    if (hash == H_ENDING && !g_neonUnlocked) {
        g_neonUnlocked = 1; save_unlocks();
        clog("career finished (ending scene) - custom neon colours unlocked");
    }
}
__declspec(naked) static void h_nis(void)
{
    __asm {
        push ecx
        push dword ptr [esp + 0xc]
        call nis_started
        add esp, 4
        pop ecx
        jmp dword ptr [o_nis]
    }
}
/* In-game gauges: the race HUD (0x4f5454..) reads the colour parts a4 (backing), a5 (needle), a6 (numbers) into its own
   fields (+0x90 B,G,R,A backing, +0xa0 needle, +0xb0 numbers) and applies them right after 0x4f5977 - stock gauge too.
   The plugin overwrites those fields there with the custom colours of the player's car. */
#define A_HUDAPPLY 0x4f5977
/* Extra gauge parts (no colour slot in the game): FE objects of the race HUD package, found by name with the game's own
   FindObject 0x537a60(package, FEHash 0x505450(name)) and coloured with 0x51d860(object, {B,G,R,A} 0..255) every frame of a
   drive (their own alpha is kept: hidden parts stay hidden). Package = the tachometer's +8, taken at 0x4f5977 above.
   Names from the tachometer / nitrous gauge set-up code (0x4f5142.., strings 0x7985xx-0x7988xx). */
typedef void (__cdecl *PFN_FESetCol)(void *obj, DWORD *col);
typedef void *(__cdecl *PFN_FEFind)(void *pkg, DWORD hash);
typedef DWORD (__cdecl *PFN_FEHash)(const char *s);
static const PFN_FESetCol FE_SetColour = (PFN_FESetCol)0x51d860;
static const PFN_FEFind FE_Find = (PFN_FEFind)0x537a60;
static const PFN_FEHash FE_Hash = (PFN_FEHash)0x505450;
static const struct { int slot; const char *obj; } HUDX[] = {
    { 0x1a4, "NOS_Fill" }, { 0x1a4, "NOS_Line" },
    { 0x2a4, "Redline" }, { 0x2a4, "RPM_Redline" },
    { 0x1a5, "3rdPersonSpeed" }, { 0x1a5, "3rdPersonSpeedUnits" }, { 0x1a5, "3rdPersonGear" },
    { 0x2a5, "SS_HudBacking" }, { 0x2a5, "GearBkg" },
};
#define NHUDX (int)(sizeof HUDX / sizeof HUDX[0])
static void *g_hudPkg, *g_hxObj[NHUDX]; static DWORD g_hxTry, g_hxStart; static int g_hxMiss[NHUDX], g_hxSets, g_hxReported;
static void hudx_new(void *pkg)
{
    if (pkg != g_hudPkg) { memset(g_hxObj, 0, sizeof g_hxObj); memset(g_hxMiss, 0, sizeof g_hxMiss); g_hxTry = 0; }
    g_hudPkg = pkg; g_hxStart = GetTickCount(); g_hxSets = 0; g_hxReported = 0;
}
/* HUD shop preview (G2): the Custom HUD screens show a preview gauge in their own FE packages. FindObject's first argument
   is the package NAME (0x52cef0 compares it with the loaded packages' +0xe4), so the same part names are looked up in
   these packages while a HUD shop screen is shown. Names that are not there are logged once and skipped. */
static const char *HUDX_SHOP[2] = { "UI_CustomHUDOverlay.fng", "UI_ChooseCustomHUD.fng" };
static void *g_hsObj[2][NHUDX]; static int g_hsMiss[2][NHUDX]; static DWORD g_hsTry;
static int nav_is_hud(void);
static void hudx_colour(BYTE *o, DWORD rgb)
{
    DWORD *cur = *(DWORD **)(o + 0x2c), col[4];
    col[0] = rgb & 255; col[1] = (rgb >> 8) & 255; col[2] = (rgb >> 16) & 255; col[3] = cur && readable(cur, 16) ? cur[3] : 255;
    if (cur && readable(cur, 16) && cur[0] == col[0] && cur[1] == col[1] && cur[2] == col[2]) return;
    FE_SetColour(o, col);
}
static void hudx_shop(void)
{
    int k, i, tried = 0; DWORD rgb, now = GetTickCount(); static int was;
    int on = *(int *)FLOW == 3 && g_inShop && nav_is_hud();
    if (!on) { if (was) { memset(g_hsObj, 0, sizeof g_hsObj); memset(g_hsMiss, 0, sizeof g_hsMiss); } was = 0; return; }
    if (!was) g_hsTry = 0;
    was = 1;
    __try {
        for (k = 0; k < 2; k++) for (i = 0; i < NHUDX; i++) {
            BYTE *o;
            if (!custom_for((BYTE *)FE_RIDE, HUDX[i].slot, &rgb)) continue;
            if (!g_hsObj[k][i]) {
                if (g_hsMiss[k][i] >= 3 || now - g_hsTry < 1000) continue;
                tried = 1;
                g_hsObj[k][i] = FE_Find((void *)HUDX_SHOP[k], FE_Hash(HUDX[i].obj));
                if (!g_hsObj[k][i]) { if (++g_hsMiss[k][i] == 3) clog("gauges preview: %s not in %s", HUDX[i].obj, HUDX_SHOP[k]); continue; }
                clog("gauges preview: %s found in %s (%p)", HUDX[i].obj, HUDX_SHOP[k], g_hsObj[k][i]);
            }
            o = (BYTE *)g_hsObj[k][i];
            if (!readable(o, 0x30)) { g_hsObj[k][i] = NULL; continue; }
            hudx_colour(o, rgb);
        }
        if (tried) g_hsTry = now;
    } __except (EXCEPTION_EXECUTE_HANDLER) { memset(g_hsObj, 0, sizeof g_hsObj); g_hsTry = now + 5000; clog("gauges preview: exception 0x%08lx", GetExceptionCode()); }
}
static void hudx_apply(void)
{
    int i, tried = 0; DWORD rgb, now = GetTickCount();
    if (*(int *)FLOW != 6) { g_hudPkg = NULL; if (g_enabled && g_kindOn[K_HUD]) hudx_shop(); return; }
    if (!g_hudPkg || !g_enabled || !g_kindOn[K_HUD]) return;
    __try {
        for (i = 0; i < NHUDX; i++) {
            BYTE *o; DWORD *cur, col[4];
            if (!custom_for((BYTE *)FE_RIDE, HUDX[i].slot, &rgb)) continue;
            if (!g_hxObj[i]) {
                if (g_hxMiss[i] >= 5 || now - g_hxTry < 1000) continue;
                tried = 1;
                g_hxObj[i] = FE_Find(g_hudPkg, FE_Hash(HUDX[i].obj));
                if (!g_hxObj[i]) { if (++g_hxMiss[i] == 5) clog("gauges: %s not in this HUD", HUDX[i].obj); continue; }
                o = (BYTE *)g_hxObj[i]; cur = readable(o, 0x30) ? *(DWORD **)(o + 0x2c) : NULL;
                if (cur && readable(cur, 16)) clog("gauges: %s found (%p), its colour B%lu G%lu R%lu A%lu", HUDX[i].obj, o, cur[0], cur[1], cur[2], cur[3]);
            }
            o = (BYTE *)g_hxObj[i];
            if (!readable(o, 0x30)) { g_hxObj[i] = NULL; continue; }
            cur = *(DWORD **)(o + 0x2c);
            col[0] = rgb & 255; col[1] = (rgb >> 8) & 255; col[2] = (rgb >> 16) & 255; col[3] = cur && readable(cur, 16) ? cur[3] : 255;
            if (cur && readable(cur, 16) && cur[0] == col[0] && cur[1] == col[1] && cur[2] == col[2]) continue;
            FE_SetColour(o, col); g_hxSets++;
        }
        if (tried) g_hxTry = now;
        if (!g_hxReported && now - g_hxStart > 5000) { g_hxReported = 1; clog("gauges: extra parts coloured %d time(s) in the first 5 s of the drive", g_hxSets); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_hudPkg = NULL; clog("gauges: extra parts - exception 0x%08lx, stopped for this drive", GetExceptionCode()); }
}
static void *o_hudapply;
static void __cdecl hud_ingame(BYTE *hud)
{
    static const int S[3] = { 0xa4, 0xa5, 0xa6 }, O[3] = { 0x90, 0xa0, 0xb0 }; int i; DWORD rgb;
    if (!g_enabled || !g_kindOn[K_HUD] || !readable(hud, 0xc0)) return;
    hudx_new(*(void **)(hud + 8));
    for (i = 0; i < 3; i++) if (custom_for((BYTE *)FE_RIDE, S[i], &rgb)) {
        DWORD *f = (DWORD *)(hud + O[i]); f[0] = rgb & 255; f[1] = (rgb >> 8) & 255; f[2] = (rgb >> 16) & 255;
        clog("gauges: %s %06lX", SLOTS[slot_index(S[i])].label, rgb);
    }
}
__declspec(naked) static void h_hudapply(void)
{
    __asm {
        pushad
        push esi
        call hud_ingame
        add esp, 4
        popad
        jmp dword ptr [o_hudapply]
    }
}
/* Nitrous purge: 0x48b920(list, cache, hue, sat, lum) recolours the purge emitters of a car (hood list cri+0x4c0, wheel
   list cri+0x4c8): every colour of the emitters (first list node +0x1c -> +0xa0, 4 x RGBA 0..255) is shifted in HSL by
   hue/sat/lum (values from HOODHUE/HOODSAT/HOODLUM, TIREHUE/..). The plugin picks the shifts that turn the first colour into
   the custom one (the others keep their offsets), and re-runs it when the custom colour changes. */
#define A_PURGE 0x48b920
typedef void (__cdecl *PFN_Purge)(BYTE *, DWORD, float, float, float);
static void *o_purge;
typedef struct { BYTE *dst, *cri; DWORD cache; float h, s, l; int which; } PurgeCall;
static PurgeCall g_pc[8]; static LONG g_purgeSeen = -1; static int g_inPurge; static int g_pcPend[8]; static DWORD g_pcSeenT[8];
static void rgb2hsl(float r, float g, float b, float *h, float *s, float *l)
{
    float mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
    *l = (mx + mn) * 0.5f;
    if (d < 1e-5f) { *h = 0; *s = 0; return; }
    *s = *l > 0.5f ? d / (2.f - mx - mn) : d / (mx + mn);
    if (mx == r) *h = (g - b) / d + (g < b ? 6.f : 0.f); else if (mx == g) *h = (b - r) / d + 2.f; else *h = (r - g) / d + 4.f;
    *h /= 6.f;
}
static int purge_target(BYTE *dst, BYTE **criOut, int *which)
{
    int k;
    for (k = 0; k < 2; k++) {
        BYTE *cri = dst - (k ? 0x4c8 : 0x4c0), *ride;
        if (!readable(cri, 8)) continue;
        ride = *(BYTE **)(cri + 4);
        if (!readable(ride, 0x590 + 170 * 4) || !car_name(ride) || !mine(ride)) continue;
        *criOut = cri; *which = k; return 1;
    }
    return 0;
}
static void purge_restore(BYTE *dst);
static void __cdecl purge_adjust(DWORD *a)
{
    BYTE *dst = (BYTE *)a[0], *cri, *node, *em; int which, i; DWORD rgb; float h0, s0, l0, th, ts, tl, *c;
    if (!g_enabled || !g_kindOn[K_PURGE] || !readable(dst, 4) || !purge_target(dst, &cri, &which)) return;
    __try { purge_restore(dst); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!g_inPurge) {                                         /* remember the game's own call for later re-runs */
        for (i = 0; i < 8 && g_pc[i].dst && g_pc[i].dst != dst; i++);
        if (i == 8) i = 0;
        g_pc[i].dst = dst; g_pc[i].cri = cri; g_pc[i].cache = a[1]; g_pc[i].which = which; g_pcPend[i] = 0; g_pcSeenT[i] = GetTickCount();
        g_pc[i].h = *(float *)&a[2]; g_pc[i].s = *(float *)&a[3]; g_pc[i].l = *(float *)&a[4];
    }
    return;                                                   /* colours are set after the game's call (purge_post) */
    if (!custom_for(*(BYTE **)(cri + 4), which ? PURGE_WHEEL : PURGE_HOOD, &rgb)) return;
    node = *(BYTE **)dst;
    if (!readable(node, 0x20) || node == dst) return;
    em = *(BYTE **)(node + 0x1c);
    if (!readable(em, 0xb0)) return;
    c = (float *)(em + 0xa0);
    rgb2hsl(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, &h0, &s0, &l0);
    rgb2hsl(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, &th, &ts, &tl);
    th -= h0; while (th < 0) th += 1.f; while (th >= 1.f) th -= 1.f; if (th > 0.5f) th -= 1.f;
    ts -= s0; tl -= l0;
    *(float *)&a[2] = th; *(float *)&a[3] = max(-1.f, min(1.f, ts)); *(float *)&a[4] = max(-1.f, min(1.f, tl));
    clog("purge %s: car %p -> %06lX (shift h %.3f s %.3f l %.3f)", which ? "wheels" : "hood", cri, rgb, th, ts, tl);
}
/* after 0x48b920(list, cache, hue, sat, lum) recoloured a stream: its emitters (list nodes, +0x1c -> +0xa0, 4 colours RGBA
   along the particle's life) get colour 1 -> colour 2 of the custom purge */
/* our direct colours must never become the base of the game's next recolour (the hood streams recolour from the emitter's
   own colours: a stock row after CUSTOM showed our hood colour shifted). Before every game recolour the emitters we wrote
   get their own colours back; after it we write ours again only when the custom purge is shown. */
typedef struct { BYTE *em; float orig[16], mine[16]; } PEm;
static PEm g_pem[64]; static int g_npem;
static void purge_restore(BYTE *dst)
{
    BYTE *node, *em; int n = 0, i;
    for (node = *(BYTE **)dst; node && node != dst && readable(node, 0x20) && n < 16; node = *(BYTE **)node, n++) {
        float *c;
        em = *(BYTE **)(node + 0x1c);
        if (!readable(em, 0xe0)) continue;
        c = (float *)(em + 0xa0);
        for (i = 0; i < g_npem && g_pem[i].em != em; i++);
        if (i < g_npem) {
            if (!memcmp(c, g_pem[i].mine, 64)) memcpy(c, g_pem[i].orig, 64);   /* still our colours -> the emitter's own back */
            else memcpy(g_pem[i].orig, c, 64);                                   /* the game changed it since: that is its own */
        } else { if (g_npem == 64) g_npem = 0; g_pem[g_npem].em = em; memcpy(g_pem[g_npem].orig, c, 64); memset(g_pem[g_npem].mine, 0xff, 64); g_npem++; }
    }
}
static void purge_mark(BYTE *em)           /* remember what we wrote into an emitter */
{
    int i; for (i = 0; i < g_npem && g_pem[i].em != em; i++);
    if (i < g_npem) memcpy(g_pem[i].mine, em + 0xa0, 64);
}
static void __cdecl purge_post(DWORD *a)
{
    BYTE *dst = (BYTE *)a[0], *cri, *ride, *node, *em; int which, n = 0, k, has1, has2; DWORD c1 = 0, c2 = 0;
    if (!g_enabled || !g_kindOn[K_PURGE] || !readable(dst, 4) || !purge_target(dst, &cri, &which)) return;
    ride = *(BYTE **)(cri + 4);
    has1 = custom_for(ride, which ? PURGE_WHEEL : PURGE_HOOD, &c1);
    has2 = custom_for(ride, which ? PURGE_WHEEL2 : PURGE_HOOD2, &c2);
    if (!has1 && !has2) return;
    for (node = *(BYTE **)dst; node && node != dst && readable(node, 0x20) && n < 16; node = *(BYTE **)node, n++) {
        float *c, sc; DWORD a1, a2;
        em = *(BYTE **)(node + 0x1c);
        if (!readable(em, 0xe0)) continue;
        c = (float *)(em + 0xa0);
        sc = (c[0] <= 1.f && c[1] <= 1.f && c[2] <= 1.f && c[4] <= 1.f && c[5] <= 1.f) ? 1.f / 255.f : 1.f;   /* 0..1 or 0..255 */
        a1 = has1 ? c1 : ((DWORD)(c[0] / sc) << 16) | ((DWORD)(c[1] / sc) << 8) | (DWORD)(c[2] / sc);
        a2 = has2 ? c2 : a1;
        for (k = 0; k < 4; k++) {
            float f = k / 3.f, r = ((a1 >> 16) & 255) * (1 - f) + ((a2 >> 16) & 255) * f, g = ((a1 >> 8) & 255) * (1 - f) + ((a2 >> 8) & 255) * f,
                  b = (a1 & 255) * (1 - f) + (a2 & 255) * f;
            c[k * 4] = r * sc; c[k * 4 + 1] = g * sc; c[k * 4 + 2] = b * sc;            /* alpha (+3) stays the game's */
        }
        purge_mark(em);
    }
    { static DWORD last; if (GetTickCount() - last > 2000) { last = GetTickCount(); clog("purge %s: %d emitter(s) -> %06lX / %06lX", which ? "wheels" : "hood", n, has1 ? c1 : 0, has2 ? c2 : (has1 ? c1 : 0)); } }
}
__declspec(naked) static void h_purge(void)
{
    __asm {
        lea eax, [esp + 4]
        push eax
        call purge_adjust               /* remembers the call (for re-runs when the colour changes) */
        add esp, 4
        push dword ptr [esp + 0x14]
        push dword ptr [esp + 0x14]
        push dword ptr [esp + 0x14]
        push dword ptr [esp + 0x14]
        push dword ptr [esp + 0x14]
        call dword ptr [o_purge]        /* the game's recolour (cdecl, 5 arguments) */
        add esp, 0x14
        lea eax, [esp + 4]
        push eax
        call purge_post
        add esp, 4
        ret
    }
}
static int purge_wheels(void) { return SLOTS[g_sel].slot == PURGE_WHEEL || SLOTS[g_sel].slot == PURGE_WHEEL2; }
static int purge_col2(void) { return SLOTS[g_sel].slot == PURGE_HOOD2 || SLOTS[g_sel].slot == PURGE_WHEEL2; }
static void purge_refresh(void)                     /* custom purge colour changed: recolour the remembered emitters */
{
    LONG v = g_purgeVer; int i; DWORD now = GetTickCount();
    if (!o_purge) return;
    if (v != g_purgeSeen) { g_purgeSeen = v; for (i = 0; i < 8; i++) g_pcPend[i] = g_pc[i].dst != NULL; }
    for (i = 0; i < 8; i++) {
        if (!g_pc[i].dst) continue;
        if (!cri_live(g_pc[i].cri)) {            /* E2: never touch a car the game did not draw this frame (it may be freed) */
            if (now - g_pcSeenT[i] > 2000) { g_pc[i].dst = NULL; g_pcPend[i] = 0; }
            continue;
        }
        g_pcSeenT[i] = now;
        if (!g_pcPend[i]) continue;
        g_pcPend[i] = 0;
        if (!*(BYTE **)g_pc[i].dst) continue;
        __try { g_inPurge = 1; ((PFN_Purge)A_PURGE)(g_pc[i].dst, g_pc[i].cache, g_pc[i].h, g_pc[i].s, g_pc[i].l); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_pc[i].dst = NULL; }
        g_inPurge = 0;
    }
}
__declspec(naked) static void h_browsernav(void)
{
    __asm {
        mov g_browser, ecx
        jmp dword ptr [o_browsernav]
    }
}
/* K / L while driving: orbit the player camera around the car. Camera::SetCameraMatrix 0x447280 thiscall(view matrix*,
   dt) ret 8 (copies the new world->view matrix into Camera+0; Camera+0x60 = look target). The player camera is
   eViews 0x832de0 + 1 * 0x70, +0x40. New view = T(-t) * Rz(-yaw) * T(t) * V (row vectors, Z up). K + L together = back. */
#define A_SETCAM 0x447280
static void *o_setcam; static float g_camYaw, g_camM[16]; static DWORD g_camT;
static void mat_mul(const float *a, const float *b, float *o)
{
    int r, c, k; float t[16];
    for (r = 0; r < 4; r++) for (c = 0; c < 4; c++) { float v = 0; for (k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c]; t[r * 4 + c] = v; }
    memcpy(o, t, 64);
}
static float g_orbR, g_orbH, g_orbIniR, g_orbIniH = 0.f, g_orbW, g_orbA0;   /* g_orbA0 = camera angle round the car when K / L went down */     /* radius / height (0 in ini = take the game camera's) */
static int player_car_pos(float *pos)
{
    BYTE *pl = *(BYTE **)0x8900b4, *car;
    if (!readable(pl, 8)) return 0;
    car = *(BYTE **)(pl + 4); if (!readable(car, 0x70)) return 0;
    pos[0] = *(float *)(car + 0x60); pos[1] = *(float *)(car + 0x64); pos[2] = *(float *)(car + 0x68);
    return pos[0] == pos[0] && pos[0] > -1e6f && pos[0] < 1e6f && pos[1] > -1e6f && pos[1] < 1e6f;
}
/* Orbit camera: eye on a circle of fixed radius / height around the car, always looking at the car, angle = the game
   camera's own angle + yaw (so it follows the car's heading). Built with the game's look-at 0x5ba9a0(out, eye, target, up).
   While K / L are held the yaw changes; released, the camera blends back to the game's camera in ~0.5 s. */
static float *__cdecl cam_adjust(BYTE *cam, float *m)
{
    BYTE *view = *(BYTE **)(0x832de0 + 0x70 + 0x40); DWORD now = GetTickCount(); float dt, c[3], e[3], a0, d, R, H; int fg, k, l, i;
    dt = g_camT ? (now - g_camT) / 1000.f : 0; if (dt > 0.1f) dt = 0.1f; g_camT = now;
    if (!g_enabled || !g_turnOn || *(int *)FLOW != 6 || cam != view || !readable(m, 64) || !player_car_pos(c)) { g_camYaw = 0; g_orbW = 0; return m; }
    fg = game_front(); k = fg && (GetAsyncKeyState('K') & 0x8000); l = fg && (GetAsyncKeyState('L') & 0x8000);
    for (i = 0; i < 3; i++) e[i] = -(m[12] * m[i * 4 + 0] + m[13] * m[i * 4 + 1] + m[14] * m[i * 4 + 2]);   /* game camera eye */
    d = sqrtf((e[0] - c[0]) * (e[0] - c[0]) + (e[1] - c[1]) * (e[1] - c[1]));
    if (k || l) {
        float sp = (fg && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) ? 3.4f : 1.6f;
        if (g_orbW == 0) {                                   /* start: fixed gap to the car body and fixed height */
            g_orbR = g_orbIniR > 0.2f ? g_orbIniR : 2.0f;    /* metres from the body (ini OrbitDistance) */
            g_orbH = g_orbIniH > 0.2f ? g_orbIniH : 1.2f;    /* metres above the car centre (ini OrbitHeight) */
            g_orbA0 = atan2f(e[1] - c[1], e[0] - c[0]);
            clog("orbit: start car %.1f %.1f %.1f, game eye %.1f %.1f %.1f (dist %.2f), angle %.0f deg, gap %.1f height %.1f",
                 c[0], c[1], c[2], e[0], e[1], e[2], d, g_orbA0 * 57.2958f, g_orbR, g_orbH);    /* the game's chase angle, taken ONCE: the game builds its next
                                                                camera from the one we set, so reading it every frame added
                                                                the yaw again each frame (camera spun / jumped to the front) */
        }
        if (k) g_camYaw += sp * dt;
        if (l) g_camYaw -= sp * dt;
        g_orbW = min(1.f, g_orbW + dt * 6.f);
    } else if (g_orbW > 0) {
        g_orbW = max(0.f, g_orbW - dt * 2.f);                /* smooth way back */
        if (g_orbW == 0) g_camYaw = 0;
    }
    if (g_orbW <= 0) return m;
    if (g_camYaw > 3.14159265f) g_camYaw -= 6.2831853f;
    if (g_camYaw < -3.14159265f) g_camYaw += 6.2831853f;
    a0 = g_orbA0;
    {
        float w = g_orbW * g_orbW * (3 - 2 * g_orbW), yaw = g_camYaw * w;   /* eased */
        /* the same gap to the body all round: an oval around the car (half length ~2.2 m, half width ~0.95 m); the game
           camera sits behind the car, so g_camYaw is the angle from the car's tail */
        float ca = cosf(g_camYaw), sa = sinf(g_camYaw), A = 2.2f + g_orbR, B = 0.95f + g_orbR;
        float Ro = 1.f / sqrtf(ca * ca / (A * A) + sa * sa / (B * B));
        R = Ro * w + d * (1 - w); H = g_orbH * w + (e[2] - c[2]) * (1 - w);
        float eye[3] = { c[0] + R * cosf(a0 + yaw), c[1] + R * sinf(a0 + yaw), c[2] + H }, tgt[3] = { c[0], c[1], c[2] + 0.35f * w + (1 - w) * 0.5f }, up[3] = { 0, 0, 1 };
        typedef void (__cdecl *F)(float *, float *, float *, float *);
        ((F)0x5ba9a0)(g_camM, eye, tgt, up);
        return g_camM;
    }
}
__declspec(naked) static void h_setcam(void)
{
    __asm {
        push ecx
        push dword ptr [esp + 8]
        push ecx
        call cam_adjust
        add esp, 8
        pop ecx
        mov [esp + 4], eax
        jmp dword ptr [o_setcam]
    }
}
/* Rainbow dot on the game's "Custom color" button, in the 3 spaces the label starts with. The PC nav bar sits at a fixed
   place of the 640x480 menu layout: the install button (where our label goes) spans x 472..614, y 434..450 (measured on
   3440x1440 shop screenshots, 16.11/17.11); its text is centred, "Preset colors" is 63 units wide there. */
/* ---------------- Neon settings panel (neon shop screens) ----------------
   Sliders for the neon glow (NFSU2_HDToggle) and the neon ground light (NFSU2_Neon). Moving a slider writes
   NFSU2_NeonTune.ini [NeonTune] right away (at most every 0.12 s); those plugins re-read the file when it changes.
   Mouse only (the arrow keys stay with the shop). Click the "Neon settings" button / the title bar to open / close. */
#define NS_N 11
/* NeonBrightness .. BayGlowBrightness (17.11): read by NFSU2_Neon in HD mode, 1 = as the game, 0 = off */
static const char *NS_KEY[NS_N] = { "NeonBrightness", "NeonWidth", "GlowNeonStrength", "GlowRadius", "GroundLightBrightness", "PoolSize",
                                    "PoolStretch", "PoolSide", "PoolBrightness", "PoolColourLevel", "BayGlowBrightness" };
static const char *NS_LBL[NS_N] = { "Neon tubes brightness", "Neon tube width", "Glow strength", "Glow range", "Floor light (car shape)",
                                    "Ground light size", "Ground light length", "Ground light side width", "Ground light brightness",
                                    "Ground light colour level", "Engine / trunk floor glow" };
static const float NS_LO[NS_N] = { 0.f, 0.1f, 0.f, 0.f, 0.f, 0.3f, 0.3f, 0.3f, 0.f, 0.f, 0.f };
static const float NS_HI[NS_N] = { 2.f, 5.f, 2.f, 2.f, 2.f, 4.f, 3.f, 3.f, 1.5f, 255.f, 2.f };
static const float NS_DEF[NS_N] = { 1.f, 1.f, 0.5f, 0.6f, 1.f, 1.9f, 1.5f, 1.0f, 0.3f, 144.f, 1.f };
static const float NS_STEP[NS_N] = { 0.01f, 0.05f, 0.01f, 0.01f, 0.01f, 0.05f, 0.05f, 0.05f, 0.01f, 1.f, 0.01f };
static float g_nsV[NS_N], g_nsTrk[NS_N][4], g_nsRow[NS_N][4], g_nsReset[4], g_nsSave[4], g_nsTitle[4];
static int g_nsOn = 1, g_nsOpen, g_nsLoaded, g_nsDirty; static DWORD g_nsWriteT, g_nsSeenT; static char g_nsFile[MAX_PATH];
static void ns_load(void)
{
    char b[64]; int i;
    sprintf(g_nsFile, "%s\\NFSU2_NeonTune.ini", g_dir);
    for (i = 0; i < NS_N; i++) {
        GetPrivateProfileStringA("NeonTune", NS_KEY[i], "", b, sizeof b, g_nsFile);
        g_nsV[i] = b[0] ? (float)atof(b) : NS_DEF[i];
        if (g_nsV[i] < NS_LO[i]) g_nsV[i] = NS_LO[i]; if (g_nsV[i] > NS_HI[i]) g_nsV[i] = NS_HI[i];
    }
    g_nsLoaded = 1;
}
static void ns_write(void)
{
    char tmp[MAX_PATH]; FILE *f; int i;
    g_nsDirty = 0; g_nsWriteT = GetTickCount();
    sprintf(tmp, "%s.tmp", g_nsFile);
    if (!(f = fopen(tmp, "w"))) { clog("neon settings: cannot write %s", tmp); return; }
    fprintf(f, "; NFSU2 neon settings - written by NFSU2_CustomColor (Neon settings panel in the neon shop).\n");
    fprintf(f, "; Glow* = neon glow (NFSU2_HDToggle), Pool* = neon ground light (NFSU2_Neon). Read again when this file changes.\n[NeonTune]\n");
    for (i = 0; i < NS_N; i++) fprintf(f, NS_STEP[i] >= 1.f ? "%s=%.0f\n" : "%s=%.3f\n", NS_KEY[i], g_nsV[i]);
    fclose(f);
    if (!MoveFileExA(tmp, g_nsFile, MOVEFILE_REPLACE_EXISTING)) clog("neon settings: cannot replace %s (%lu)", g_nsFile, GetLastError());
}
static int ns_due(void)
{
    BYTE *nav = g_anyNav; char *pk; int on = 0; static int was;
    if (g_nsOn && g_enabled && *(int *)FLOW == 3 && nav && readable(nav, 0x70)) {
        pk = *(char **)(nav + 0x58);
        on = readable(pk, 8) && strstr(pk, "Neon") && *(BYTE *)(nav + 0x6c);
    }
    if (on && !was) g_nsSeenT = GetTickCount();
    was = on;
    return on && GetTickCount() - g_nsSeenT > 350;
}
static void draw_arrow(IDirect3DDevice9 *d, float mx, float my, float k)    /* mouse arrow: black outline, white inside */
{
    float hgt = 13.f * k, row, wo, wi; int r, n = (int)hgt, t;
    if (n < 8) n = 8;
    for (r = 0; r < n; r++) {
        row = (float)r; wo = row * 0.62f + 1.f;
        if (row > hgt * 0.78f) wo = (hgt - row) * 2.6f + 1.f;
        if (wo < 1.f) wo = 1.f;
        quad(d, mx - 0.5f, my + row, wo + 1.f, 1.f, 0xFF000000);
        wi = wo - 2.f;
        if (wi > 0 && r > 1 && row < hgt - 2) quad(d, mx + 0.5f, my + row, wi, 1.f, 0xFFFFFFFF);
    }
    for (t = 0; t < (int)(5.5f * k); t++) {
        quad(d, mx + hgt * 0.30f + t * 0.45f - 1.f, my + hgt * 0.70f + t, 1.6f * k + 2.f, 1.f, 0xFF000000);
        quad(d, mx + hgt * 0.30f + t * 0.45f, my + hgt * 0.70f + t, 1.6f * k, 1.f, 0xFFFFFFFF);
    }
}
static void ns_panel(IDirect3DDevice9 *d, int W, int H)
{
    float s, ox, oy, x0, y0, pw, ts, mx, my, ry, rh; int i; char buf[32];
    if (!ns_due()) {
        g_nsBtn[2] = g_nsPan[2] = 0; g_nsDrag = -1; g_nsOpen = 0; g_nsPress = 0;
        if (g_nsDirty) ns_write();
        return;
    }
    if (!g_nsLoaded) ns_load();
    if (g_fontDev != d) { if (g_font) IDirect3DTexture9_Release(g_font); g_font = NULL; make_font(d); }
    if ((float)W / H >= 4.f / 3.f) { s = H / 480.f; ox = (W - 640.f * s) / 2; oy = 0; }
    else { s = W / 640.f; ox = 0; oy = (H - 480.f * s) / 2; }
    g_nsW = W; g_nsH = H;
    mx = g_mx / 10000.f * W; my = g_my / 10000.f * H;
    ts = 8.f * s / g_fontH;
    /* clicks (the press edge comes from the mouse poll) */
    if (g_nsPress) {
        g_nsPress = 0;
        if (!g_nsOpen) { if (in_r4(g_nsBtn, mx, my)) { g_nsOpen = 1; clog("neon settings: opened"); } }
        else if (in_r4(g_nsTitle, mx, my)) { g_nsOpen = 0; if (g_nsDirty) ns_write(); }
        else if (in_r4(g_nsReset, mx, my)) { for (i = 0; i < NS_N; i++) g_nsV[i] = NS_DEF[i]; ns_write(); clog("neon settings: reset to default"); }
        else if (in_r4(g_nsSave, mx, my)) { ns_write(); g_nsOpen = 0; clog("neon settings: saved"); }
        else for (i = 0; i < NS_N; i++) if (in_r4(g_nsRow[i], mx, my)) { g_nsDrag = i; break; }
    }
    if (g_nsDrag >= 0) {
        if (!g_nsHeld || !g_nsOpen) { g_nsDrag = -1; if (g_nsDirty) ns_write(); }
        else {
            int k = g_nsDrag; float f = (mx - g_nsTrk[k][0]) / g_nsTrk[k][2], v;
            if (f < 0.015f) f = 0; if (f > 0.985f) f = 1;      /* the ends of the track = exactly the limits (0 = off) */
            v = NS_LO[k] + (NS_HI[k] - NS_LO[k]) * f;
            v = (float)floor(v / NS_STEP[k] + 0.5f) * NS_STEP[k];
            if (f == 0) v = NS_LO[k]; if (f == 1) v = NS_HI[k];
            if (v != g_nsV[k]) { g_nsV[k] = v; g_nsDirty = 1; }
        }
    }
    if (g_nsDirty && GetTickCount() - g_nsWriteT > 120) ns_write();
    /* drawing: right side, under the Visual Rating box */
    pw = 204 * s; x0 = ox + 428 * s; y0 = oy + 208 * s;
    if (!g_nsOpen) {
        g_nsPan[2] = 0;
        g_nsBtn[2] = 80 * s; g_nsBtn[3] = 14 * s; g_nsBtn[0] = x0 + pw - g_nsBtn[2]; g_nsBtn[1] = y0;
        button(d, g_nsBtn[0], g_nsBtn[1], g_nsBtn[2], g_nsBtn[3], s, 0);
        text(d, "Neon settings", g_nsBtn[0] + (g_nsBtn[2] - text_w("Neon settings", 9.f * s / g_fontH)) / 2, g_nsBtn[1] + 1.5f * s, 9.f * s / g_fontH, 0xFFFFFFFF);
    } else {
        g_nsBtn[2] = 0;
        rh = 10 * s;
        g_nsPan[0] = x0; g_nsPan[1] = y0; g_nsPan[2] = pw; g_nsPan[3] = (16 + NS_N * 10 + 17) * s;
        quad(d, x0 - 2 * s, y0 - 2 * s, pw + 4 * s, g_nsPan[3] + 4 * s, 0xF0141418);
        frame(d, x0, y0, pw, g_nsPan[3], 1.f * s, 0xFF4A4A52);
        g_nsTitle[0] = x0; g_nsTitle[1] = y0; g_nsTitle[2] = pw; g_nsTitle[3] = 14 * s;
        button(d, x0 + 1 * s, y0 + 1 * s, pw - 2 * s, 13 * s, s, 1);
        text(d, "Neon settings", x0 + 6 * s, y0 + 2 * s, 9.f * s / g_fontH, GAME_GREEN);
        text(d, "close", x0 + pw - 6 * s - text_w("close", ts), y0 + 2.5f * s, ts, 0xFF9A9AA6);
        for (i = 0; i < NS_N; i++) {
            float tx = x0 + 100 * s, tw = 58 * s, f = (g_nsV[i] - NS_LO[i]) / (NS_HI[i] - NS_LO[i]);
            ry = y0 + 16 * s + i * rh;
            g_nsRow[i][0] = x0; g_nsRow[i][1] = ry; g_nsRow[i][2] = pw; g_nsRow[i][3] = rh;
            g_nsTrk[i][0] = tx; g_nsTrk[i][1] = ry; g_nsTrk[i][2] = tw; g_nsTrk[i][3] = rh;
            if (i == g_nsDrag) quad(d, x0 + 1 * s, ry, pw - 2 * s, rh, 0x40C8E870);
            text(d, NS_LBL[i], x0 + 6 * s, ry + 1 * s, ts, i == g_nsDrag ? GAME_GREEN : 0xFFFFFFFF);
            quad(d, tx, ry + rh / 2 - 1 * s, tw, 2 * s, 0xFF4A4A52);
            quad(d, tx, ry + rh / 2 - 1 * s, tw * f, 2 * s, 0xFFC8E870);
            quad(d, tx + tw * f - 1.5f * s, ry + 1.5f * s, 3 * s, rh - 3 * s, 0xFFFFFFFF);
            if (!strcmp(NS_KEY[i], "PoolColourLevel") && g_nsV[i] < 0.5f) strcpy(buf, "game colour");
            else if (NS_LO[i] == 0.f && g_nsV[i] == 0.f) strcpy(buf, "off");
            else sprintf(buf, NS_STEP[i] >= 1.f ? "%.0f" : "%.2f", g_nsV[i]);
            text(d, buf, x0 + pw - 6 * s - text_w(buf, ts), ry + 1 * s, ts, 0xFFFFFFFF);
        }
        ry = y0 + (16 + NS_N * 10 + 3) * s;
        g_nsReset[0] = x0 + 6 * s; g_nsReset[1] = ry; g_nsReset[2] = 60 * s; g_nsReset[3] = 12 * s;
        g_nsSave[2] = 44 * s; g_nsSave[3] = 12 * s; g_nsSave[0] = x0 + pw - 6 * s - g_nsSave[2]; g_nsSave[1] = ry;
        button(d, g_nsReset[0], g_nsReset[1], g_nsReset[2], g_nsReset[3], s, 0);
        text(d, "Reset to default", g_nsReset[0] + (g_nsReset[2] - text_w("Reset to default", ts)) / 2, ry + 1.5f * s, ts, 0xFFFFFFFF);
        button(d, g_nsSave[0], g_nsSave[1], g_nsSave[2], g_nsSave[3], s, 0);
        text(d, "Save", g_nsSave[0] + (g_nsSave[2] - text_w("Save", ts)) / 2, ry + 1.5f * s, ts, 0xFFFFFFFF);
    }
    if ((g_nsOver || g_nsDrag >= 0) && g_mx >= 0) draw_arrow(d, mx, my, s * 0.9f);
}
/* ---------------- "CUSTOM" row at the end of the game's neon list (N2, 17.11) ----------------
   UI_NeonPartsOverlay (vtable 0x79cae0): 0x53ba00 (thiscall screen) fills its icon list at screen+0x50 (list nodes at +4 of
   each row, head +0x54/+0x58, the selected row at +0x5c) with one row per neon part of the slot [[screen+0x110]+0x48]:
   row = 0x30 bytes, vtable 0x79afdc, +0xc / +0x2c = the part, added by 0x52ed80(list, row, 1, -1), count screen+0x10c.
   We add one more row the same way after it. Under it is a real part (the fitted one), so installing it and the save stay
   valid. Row selected = our picker opens on that neon (custom colour shown); another row selected = picker closed (the
   game's colours shown); installing a game row removes the custom colour. The bottom buttons stay the game's own. */
#define A_NEONLIST 0x53ba00
#define A_LISTADD  0x52ed80
typedef void *(__cdecl *PFN_GameNew)(DWORD size);
typedef void *(__fastcall *PFN_ListAddR)(void *list, void *edx, void *row, int a, int b);   /* returns the row's icon image */
static void *g_nlTramp, *g_plTramp; static BYTE *g_nlItem; static int g_nlSlot, g_nlOn = 1, g_nlKind; static DWORD g_nlCur;
static int g_nlWant; static DWORD g_nlAddT; static BYTE *g_nlKey, *g_nlPart;   /* open the list on CUSTOM when a custom colour is installed */
/* screen kinds with our row: 0 = neon parts overlay (vt 0x79cae0), 1 = ICE parts overlay (vt 0x79ba14, UI_IcePartsOverlay:
   the nitrous purge colours are chosen there; list built by 0x5606e0, same layout as the neon one: category [+0x110]+0x48
   = slot, list +0x50, selected row +0x5c, count +0x10c) */
static const DWORD NL_VT[2] = { 0x79cae0, 0x79ba14 }, NL_LIST[2] = { 0x50, 0x50 }, NL_CNT[2] = { 0x10c, 0x10c };
static BYTE *g_iceOv;                         /* the ICE overlay last built (any ICE category) */
static int is_ice_pk(const char *pk) { return strstr(pk, "IcePartsOverlay") != NULL || strstr(pk, "ICEMAIN") != NULL; }
static int ice_purge(void)                    /* the ICE overlay on screen shows the nitrous purge */
{
    BYTE *cat;
    if (!g_iceOv || !readable(g_iceOv, 0x120) || *(DWORD *)g_iceOv != 0x79ba14) return 0;
    cat = *(BYTE **)(g_iceOv + 0x110);
    return readable(cat, 0x50) && *(int *)(cat + 0x48) == 0xa2;
}
static int nl_pk_match(const char *pk) { return g_nlKind ? is_ice_pk(pk) : strstr(pk, "Neon") != NULL; }
/* screens where the CUSTOM row replaces our "Custom color" button: the game's own buttons stay */
static int rows_screen(const char *pk)
{
    if (!g_nlOn || !readable(pk, 8)) return 0;
    if (strstr(pk, "Neon")) return g_kindOn[K_NEON] && g_neonUnlocked;
    if (is_ice_pk(pk)) return g_kindOn[K_PURGE] && ice_purge();
    return 0;
}
static void nl_reset(void)
{
    if (g_nlOv) clog("neon list: left");
    g_nlOv = NULL; g_nlItem = NULL; g_nlFocus = 0; g_nlCur = 0;
}
/* The list's icons come from a fixed pool: the package's objects named PARTS_ICON, handed out by 0x52ec20 (list+0x28 =
   the next free one). The underglow list uses them all, so 0x52ed80 rejected (and deleted) our row. Then we make one more
   icon the way the game clones menu objects: FEObject::Clone(false) (vtable slot 1), copy its data block (+0x2c, size
   +0x30), clear the render cache (+0x58), link it into the package's object list right after the original (nodes +4 next /
   +8 prev; package = 0x52cef0(list+0x14), its object list tail at +0x50) and give it to the list as the next free icon.
   The package frees it with its other objects when the screen closes. */
typedef BYTE *(__fastcall *PFN_FEClone)(BYTE *obj, void *edx, int bReference);
typedef BYTE *(__cdecl *PFN_FindPkg)(const char *name);
static BYTE *icon_clone(BYTE *list)
{
    BYTE *last = *(BYTE **)(list + 8), *src, *c, *nx, *pkg; DWORD n;
    if (!readable(last, 8) || last == list + 4) return NULL;
    src = *(BYTE **)(last - 4 + 0x10);
    if (!readable(src, 0x5c) || !readable(*(BYTE **)src, 8)) return NULL;
    c = ((PFN_FEClone)(*(void ***)src)[1])(src, NULL, 0);
    if (!readable(c, 0x5c)) return NULL;
    n = *(DWORD *)(src + 0x30);
    if (n && n == *(DWORD *)(c + 0x30) && readable(*(BYTE **)(src + 0x2c), n) && readable(*(BYTE **)(c + 0x2c), n)) memcpy(*(BYTE **)(c + 0x2c), *(BYTE **)(src + 0x2c), n);
    *(DWORD *)(c + 0x58) = 0;
    *(DWORD *)(c + 0x1c) |= 0x400000;
    nx = *(BYTE **)(src + 4);
    *(BYTE **)(c + 4) = nx; *(BYTE **)(c + 8) = src; *(BYTE **)(src + 4) = c;
    if (nx && readable(nx, 0xc)) *(BYTE **)(nx + 8) = c;
    else if (readable(*(char **)(list + 0x14), 4) && (pkg = ((PFN_FindPkg)0x52cef0)(*(char **)(list + 0x14))) && readable(pkg, 0x58) && *(BYTE **)(pkg + 0x50) == src)
        *(BYTE **)(pkg + 0x50) = c;
    clog("custom row: list icons all used - cloned icon %p as %p", src, c);
    return c;
}
static void nl_add_(BYTE *scr, int kind)
{
    BYTE *cat, *row, *part, *n, *img; int slot; DWORD L = NL_LIST[kind];
    __try {
        if (!g_nlOn || !g_enabled || !readable(scr, 0x120) || *(DWORD *)scr != NL_VT[kind]) return;
        if (kind == 1) g_iceOv = scr;
        cat = *(BYTE **)(scr + 0x110); if (!readable(cat, 0x50)) return;
        slot = *(int *)(cat + 0x48);
        if (kind == 0 && (!g_kindOn[K_NEON] || !g_neonUnlocked || slot < 0x9a || slot > 0x9d)) return;
        if (kind == 1 && (!g_kindOn[K_PURGE] || slot != 0xa2)) return;
        part = part_of((BYTE *)FE_RIDE, slot);
        n = *(BYTE **)(scr + L + 4);                                   /* first row node -> its part, when none is fitted */
        if (!part && readable(n, 0x10) && n != scr + L + 4) part = *(BYTE **)(n - 4 + 0xc);
        if (!part) { clog("custom row: slot %02x has no part to put under CUSTOM", slot); return; }
        row = (BYTE *)((PFN_GameNew)0x575620)(0x30);
        if (!row) return;
        memset(row, 0, 0x30);
        /* +0x2c = the part the game previews / installs (the real one: saves stay valid); +0xc = the row's key, which the
           screen uses to find "the row of part X" (0x51f7a0) - with the real part there it jumped to that game row (BLUE).
           So the key is our own copy of the part (same name / attributes), found by no lookup of the game. */
        { static BYTE fk[2][16]; memcpy(fk[kind], part, 14); *(BYTE **)(row + 0xc) = fk[kind]; }
        *(BYTE **)(row + 0x2c) = part; row[0x21] = 1; *(DWORD *)row = 0x79afdc;
        if (!*(BYTE **)(scr + L + 0x28)) { BYTE *ic = icon_clone(scr + L); if (ic) *(BYTE **)(scr + L + 0x28) = ic; }
        img = (BYTE *)((PFN_ListAddR)A_LISTADD)(scr + L, NULL, row, 1, -1);
        if (!img) { clog("custom row: the list refused the row (no free icon)"); return; }      /* the game deleted it */
        (*(int *)(scr + NL_CNT[kind]))++;
        g_nlOv = scr; g_nlItem = row; g_nlSlot = slot; g_nlKind = kind; g_nlFocus = 0; g_nlCur = 0;
        g_nlKey = *(BYTE **)(row + 0xc); g_nlPart = part; g_nlAddT = GetTickCount(); g_nlWant = 0;
        { DWORD *la = o_attr(part, NULL, 0x4732da07, NULL); g_nlLangHash = la ? la[1] : 0xFFFFFFFF; }   /* LANGUAGEHASH */
        {   const char *car = car_name((BYTE *)FE_RIDE);
            if (car) { EnterCriticalSection(&g_cs);
                g_nlWant = kind ? (find_e(car, PURGE_HOOD) || find_e(car, PURGE_WHEEL) || find_e(car, PURGE_HOOD2) || find_e(car, PURGE_WHEEL2)) != 0
                                : find_e(car, slot) != NULL;
                LeaveCriticalSection(&g_cs); } }
        clog("custom row: added to the %s list (row %p, part %08lx)", kind ? "nitrous purge" : SLOTS[slot_index(slot)].label, row, part_hash(part));
    } __except (EXCEPTION_EXECUTE_HANDLER) { clog("custom row: exception 0x%08lx", GetExceptionCode()); }
}
static void __cdecl nl_add(BYTE *scr) { nl_add_(scr, 0); }
static void __cdecl pl_add(BYTE *scr) { nl_add_(scr, 1); }
__declspec(naked) static void h_neonlist(void)
{
    __asm {
        push ecx
        call dword ptr [g_nlTramp]      /* the game's list (thiscall, no arguments) */
        pop ecx
        pushad
        push ecx
        call nl_add
        add esp, 4
        popad
        ret
    }
}
__declspec(naked) static void h_purgelist(void)
{
    __asm {
        push ecx
        call dword ptr [g_plTramp]      /* the game's ICE parts list (thiscall, no arguments) */
        pop ecx
        pushad
        push ecx
        call pl_add
        add esp, 4
        popad
        ret
    }
}
static char S_ROW_CUSTOM[] = "CUSTOM", S_ROW_INST[] = "CUSTOM INSTALLED", S_DECAL_INST[] = "COLOUR INSTALLED";
static void *g_pnTramp; static DWORD g_nlLabelT, g_nlToast;
static int row_hides(int slot)                 /* stock rows of the neon / purge list preview pure stock colours */
{
    BYTE *cur;
    if (!g_nlOv || !g_nlItem || *(int *)FLOW != 3) return 0;
    if (g_nlKind == 0 ? slot != g_nlSlot : (slot & 0xFF) != 0xa2) return 0;
    if (!readable(g_nlOv, 0x120) || *(DWORD *)g_nlOv != NL_VT[g_nlKind]) return 0;
    cur = *(BYTE **)(g_nlOv + NL_LIST[g_nlKind] + 0xc);
    return cur != g_nlItem;
}
static char *__cdecl nl_lang_text(void)        /* every screen fills its name label through the language table */
{
    BYTE *cur;
    if (!g_nlOv || !g_nlItem || !readable(g_nlOv, 0x120) || *(DWORD *)g_nlOv != NL_VT[g_nlKind]) return NULL;
    cur = *(BYTE **)(g_nlOv + NL_LIST[g_nlKind] + 0xc);
    return cur != g_nlItem ? NULL : GetTickCount() - g_nlToast < 1500 ? S_ROW_INST : S_ROW_CUSTOM;
}
static char *__cdecl nl_name(BYTE *part)       /* the game asks the name of the selected row's part: our row -> CUSTOM */
{
    BYTE *cur;
    if (!g_nlOv || !g_nlItem || part != g_nlPart || !readable(g_nlOv, 0x120) || *(DWORD *)g_nlOv != NL_VT[g_nlKind]) return NULL;
    cur = *(BYTE **)(g_nlOv + NL_LIST[g_nlKind] + 0xc);
    if (cur != g_nlItem) return NULL;
    return GetTickCount() - g_nlToast < 1500 ? S_ROW_INST : S_ROW_CUSTOM;
}
__declspec(naked) static void h_partname(void)
{
    __asm {
        push ecx
        push dword ptr [esp + 8]
        call nl_name
        add esp, 4
        pop ecx
        test eax, eax
        jz orig
        ret 4
    orig:
        jmp dword ptr [g_pnTramp]
    }
}
typedef void (__cdecl *PFN_FEPrintf)(const char *pkg, DWORD obj, const char *fmt);
static void set_part_label(const char *text)   /* the game's PART_NAME text in the shop screens on now */
{
    __try {
        if (g_anyNav && readable(g_anyNav, 0x70) && readable(*(char **)(g_anyNav + 0x58), 8)) ((PFN_FEPrintf)0x537b80)(*(char **)(g_anyNav + 0x58), 0xd28b9316, text);
        if (g_nav && g_nav != g_anyNav && readable(g_nav, 0x70) && readable(*(char **)(g_nav + 0x58), 8)) ((PFN_FEPrintf)0x537b80)(*(char **)(g_nav + 0x58), 0xd28b9316, text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
/* Install on the CUSTOM row: under it is the fitted part, so the game answered "already installed" (and its dialog took the
   picker away). Install there = keep the custom colour (it is saved as it changes): the screen's PAD_ACCEPT (0x406415e3,
   also sent by the Install button) is answered by us. Message handlers (thiscall msg, obj, p1, p2; ret 0x10): neon parts
   overlay vtable 0x79cae0 [2] = 0x56a300, ICE parts overlay 0x79ba14 [2] = 0x568d30. */
static void *g_nmTramp[2];
static int __cdecl nl_msg(BYTE *scr, DWORD msg)
{
    if (msg != 0x406415e3 || !g_nlFocus || scr != g_nlOv) return 0;
    g_nlToast = GetTickCount(); g_nlLabelT = g_nlToast; set_part_label(S_ROW_INST);
    if (g_dirty) { g_dirty = 0; EnterCriticalSection(&g_cs); save_store(); LeaveCriticalSection(&g_cs); }
    clog("custom row: Install on CUSTOM - custom colour kept (the game's install skipped)");
    return 1;
}
__declspec(naked) static void h_neonmsg(void)
{
    __asm {
        push ecx
        push dword ptr [esp + 8]
        push ecx
        call nl_msg
        add esp, 8
        pop ecx
        test eax, eax
        jz orig
        ret 0x10
    orig:
        jmp dword ptr [g_nmTramp]
    }
}
__declspec(naked) static void h_icemsg(void)
{
    __asm {
        push ecx
        push dword ptr [esp + 8]
        push ecx
        call nl_msg
        add esp, 8
        pop ecx
        test eax, eax
        jz orig
        ret 0x10
    orig:
        jmp dword ptr [g_nmTramp + 4]
    }
}
static int nl_active(void)                    /* our row is in the neon / purge list on screen now */
{
    BYTE *nav = g_anyNav; char *pk;
    if (!g_nlOv) return 0;
    if (*(int *)FLOW != 3 || !readable(g_nlOv, 0x120) || *(DWORD *)g_nlOv != NL_VT[g_nlKind] || !nav || !readable(nav, 0x70)) { nl_reset(); return 0; }
    pk = *(char **)(nav + 0x58);
    if (!readable(pk, 8) || !nl_pk_match(pk)) { nl_reset(); return 0; }
    if (g_nlKind == 1 && !ice_purge()) { nl_reset(); return 0; }
    return 1;
}
static void nl_update(void)
{
    BYTE *cur; DWORD inst; int f;
    if (!nl_active()) return;
    cur = *(BYTE **)(g_nlOv + NL_LIST[g_nlKind] + 0xc);
    if (g_nlWant) {        /* the game opened the list on the fitted part's row: a custom colour is installed -> on CUSTOM */
        if (cur == g_nlItem || GetTickCount() - g_nlAddT > 3000) g_nlWant = 0;
        else if (readable(cur, 0x30) && *(BYTE **)(cur + 0xc) == g_nlPart) {
            __try { ((void (__fastcall *)(BYTE *, void *, BYTE *))0x52ee40)(g_nlOv + NL_LIST[g_nlKind], NULL, g_nlKey); } __except (EXCEPTION_EXECUTE_HANDLER) { g_nlWant = 0; }
            cur = *(BYTE **)(g_nlOv + NL_LIST[g_nlKind] + 0xc);
            if (cur == g_nlItem) { g_nlWant = 0; clog("custom row: custom colour installed - list opened on CUSTOM"); }
        }
    }
    if ((DWORD)cur != g_nlCur) {
        g_nlCur = (DWORD)cur;
        clog("custom row: selected row %p (part %08lx)%s", cur, readable(cur, 0x10) ? part_hash(*(BYTE **)(cur + 0xc)) : 0, cur == g_nlItem ? " = CUSTOM" : "");
    }
    f = cur == g_nlItem;
    inst = g_nav ? shop_nav(g_nav) : 0;
    if (f && !g_nlFocus && inst) {
        if (!g_shopMode) shop_toggle(NULL, inst);
        if (g_shopMode && g_nlKind == 0) { g_sel = slot_index(g_nlSlot); load_sel(); }
    } else if (!f && g_nlFocus && g_shopMode && inst) shop_toggle(NULL, inst);
    g_nlFocus = f;
}
static void nl_draw(IDirect3DDevice9 *d, int W, int H)     /* (labels now come from the game: see nl_name) */
{
    if (g_nlLabelT && GetTickCount() - g_nlLabelT > 1500) {                  /* "... INSTALLED" -> back to the normal name */
        g_nlLabelT = 0;
        if (g_nlFocus && nl_active()) set_part_label(S_ROW_CUSTOM);
    }
    return;
    float s, ox, oy, x0, y0, w, h, ts, r; V v[26]; int i, pass;
    if (!g_nlFocus || !nl_active()) return;
    if (g_fontDev != d) { if (g_font) IDirect3DTexture9_Release(g_font); g_font = NULL; make_font(d); }
    if ((float)W / H >= 4.f / 3.f) { s = H / 480.f; ox = (W - 640.f * s) / 2; oy = 0; } else { s = W / 640.f; ox = 0; oy = (H - 480.f * s) / 2; }
    x0 = ox + 196 * s; y0 = oy + 398 * s; w = 186 * s; h = 20 * s;          /* the name field (measured on 17.11 screenshots) */
    quad(d, x0, y0, w, h, 0xF0181C14);
    ts = 13.f * s / g_fontH;
    { float tw = text_w("CUSTOM", ts), tx = x0 + (w - tw) / 2 + 6 * s;
      if (GetTickCount() - g_nlToast < 1500) { const char *m = "CUSTOM INSTALLED"; tx = x0 + (w - text_w(m, ts)) / 2 + 6 * s; text(d, m, tx, y0 + 1.5f * s, ts, GAME_GREEN); }
      else text(d, "CUSTOM", tx, y0 + 1.5f * s, ts, GAME_GREEN);
      r = 4.5f * s;
      solid(d);
      for (pass = 0; pass < 2; pass++) {
          float rr = pass ? r : r + 0.6f * s, cx = tx - 9 * s, cy = y0 + h / 2;
          v[0].x = cx; v[0].y = cy; v[0].z = 0; v[0].rhw = 1; v[0].u = v[0].v = 0; v[0].c = pass ? 0xFFFFFFFF : 0xC0000000;
          for (i = 0; i <= 24; i++) {
              float a = i * 6.2831853f / 24;
              v[i + 1].x = cx + cosf(a) * rr; v[i + 1].y = cy - sinf(a) * rr; v[i + 1].z = 0; v[i + 1].rhw = 1; v[i + 1].u = v[i + 1].v = 0;
              v[i + 1].c = pass ? argb(hsv_rgb((float)(i % 24) * 15.f, 1, 1), 255) : 0xC0000000;
          }
          IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLEFAN, 24, v, sizeof(V));
      } }
}
/* vinyl browser: the bottom buttons are laid out once when the list opens, before we know whether a picture vinyl is shown.
   When that changes, relabel them like the paint screens: top-right = Custom color, bottom-right = Install Vinyl
   (the clicks follow shop_nav at click time). */
static void vpic_labels(void)
{
    /* the vinyl browser's bar is laid out before a picture vinyl is known, so at that moment it was not "our" screen and
       g_nav kept the previous shop bar: use the bar on screen now (g_anyNav) and adopt it once a picture vinyl shows */
    static int was; static BYTE *wasNav; int now; BYTE *nav = g_anyNav, *ni, *nq; char *pk;
    if (!nav || !readable(nav, 0x70) || !o_navlabel) return;
    pk = *(char **)(nav + 0x58);
    now = *(int *)FLOW == 3 && readable(pk, 8) && strstr(pk, "GenericParts") && (*(DWORD *)(nav + 0x64) & 0x10) && vpic_now() && g_vTint && g_enabled;
    if (nav != wasNav) { wasNav = nav; was = -1; }
    if (now == was) return;
    was = now;
    if (now) g_nav = nav;                       /* our screen now: picker / rainbow dot / clicks use this bar */
    ni = nav_node(nav, 0x10); nq = nav_node(nav, 1);
    __try {
        if (ni && readable(ni, 0x20) && *(void **)(ni + 0x10)) SetLabel(*(void **)(ni + 0x10), now ? (g_shopMode ? H_CC_PRESET : H_CC_BTN) : o_navlabel(nav, NULL, 0x10));
        if (nq && readable(nq, 0x20) && *(void **)(nq + 0x10)) SetLabel(*(void **)(nq + 0x10), o_navlabel(nav, NULL, now ? 0x10 : 1));
        clog("vinyl shop: bar relabelled (%s), nodes %p / %p", now ? "Back | Custom color / Install Vinyl" : "stock", ni, nq);
    } __except (EXCEPTION_EXECUTE_HANDLER) { clog("vinyl shop: relabel failed"); }
}
/* ---------------- D8: a real rainbow icon for the CUSTOM row ----------------
   List icons are menu images (FEImage) whose +0x24 holds a texture hash (the game itself sets it, e.g. 0x73c4ec2d, and marks
   the image dirty with +0x1c |= 0x400000); the menu finds textures through GetTextureInfo 0x4901d0 (cdecl hash, ...).
   We make one texture from the row's own dot picture: same size, format and alpha (so the exact same round shape and soft
   edge), its colour replaced by a hue wheel around the centre that fades to white in the middle, keeping the dot's shading.
   GetTextureInfo answers our hash with a TextureInfo clone that points at that texture. */
static BYTE *g_rbTI; static DWORD g_rbHash; static void *g_gtiTramp;
__declspec(naked) static void h_gettex(void)
{
    __asm {
        mov eax, [esp + 4]
        cmp eax, g_rbHash
        jne orig
        mov eax, g_rbTI
        test eax, eax
        jz orig
        ret
    orig:
        jmp dword ptr [g_gtiTramp]
    }
}
static DWORD rb_colour(float x, float y, float cx, float cy, float R, float shade)
{
    float dx = x - cx, dy = y - cy, rr = sqrtf(dx * dx + dy * dy) / R, ang = atan2f(-dy, dx) * 57.29578f, r, g, b, t;
    if (ang < 0) ang += 360.f;
    hsv2rgb(ang, 1, 1, &r, &g, &b);
    t = (rr - 0.12f) / 0.5f; if (t < 0) t = 0; if (t > 1) t = 1;        /* white centre -> full colour at the rim */
    r = (1 - t) + t * r; g = (1 - t) + t * g; b = (1 - t) + t * b;
    if (shade > 1) shade = 1;
    return ((DWORD)(r * shade * 255 + .5f) << 16) | ((DWORD)(g * shade * 255 + .5f) << 8) | (DWORD)(b * shade * 255 + .5f);
}
static WORD to565(DWORD c) { return (WORD)((((c >> 19) & 31) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31)); }
static void c565(WORD c, float *o) { o[0] = ((c >> 11) & 31) / 31.f; o[1] = ((c >> 5) & 63) / 63.f; o[2] = (c & 31) / 31.f; }
static BYTE *make_rainbow(BYTE *src)
{
    BYTE *pi, *c; IDirect3DTexture9 *tex, *nt = NULL; IDirect3DDevice9 *dev = NULL; D3DSURFACE_DESC d0; DWORD lv, L; int dxt;
    if (!readable(src, 0x7c) || !readable(pi = *(BYTE **)src, 0x20) || !(tex = *(IDirect3DTexture9 **)(pi + 0x18))) return NULL;
    if (FAILED(IDirect3DTexture9_GetLevelDesc(tex, 0, &d0))) return NULL;
    dxt = d0.Format == D3DFMT_DXT1 ? 1 : (d0.Format == D3DFMT_DXT3 || d0.Format == D3DFMT_DXT5) ? 2 : d0.Format == D3DFMT_A8R8G8B8 ? 0 : -1;
    if (dxt < 0) { clog("rainbow icon: %s format %08lx - not supported", (char *)src + 0xc, (DWORD)d0.Format); return NULL; }
    lv = IDirect3DTexture9_GetLevelCount(tex);
    IDirect3DTexture9_GetDevice(tex, &dev);
    if (!dev || FAILED(IDirect3DDevice9_CreateTexture(dev, d0.Width, d0.Height, lv, 0, d0.Format, D3DPOOL_MANAGED, &nt, NULL))) { if (dev) IDirect3DDevice9_Release(dev); return NULL; }
    IDirect3DDevice9_Release(dev);
    for (L = 0; L < lv; L++) {
        D3DLOCKED_RECT a, b; D3DSURFACE_DESC d2; int x, y, rows, cols, bs = dxt == 1 ? 8 : 16, co = dxt == 1 ? 0 : 8; float cx, cy, R, mx = 0;
        IDirect3DTexture9_GetLevelDesc(tex, L, &d2);
        if (FAILED(IDirect3DTexture9_LockRect(tex, L, &a, NULL, D3DLOCK_READONLY))) { IDirect3DTexture9_Release(nt); return NULL; }
        if (FAILED(IDirect3DTexture9_LockRect(nt, L, &b, NULL, 0))) { IDirect3DTexture9_UnlockRect(tex, L); IDirect3DTexture9_Release(nt); return NULL; }
        cx = d2.Width / 2.f; cy = d2.Height / 2.f; R = min(d2.Width, d2.Height) / 2.f;
        rows = dxt ? ((int)d2.Height + 3) / 4 : (int)d2.Height; cols = dxt ? ((int)d2.Width + 3) / 4 : (int)d2.Width;
        for (y = 0; y < rows; y++) memcpy((BYTE *)b.pBits + y * b.Pitch, (BYTE *)a.pBits + y * a.Pitch, dxt ? cols * bs : cols * 4);
        IDirect3DTexture9_UnlockRect(tex, L);
        /* brightest source colour = full brightness of the wheel */
        for (y = 0; y < rows; y++) for (x = 0; x < cols; x++) {
            if (dxt) { BYTE *cb = (BYTE *)b.pBits + y * b.Pitch + x * bs + co; float t[3]; int k; for (k = 0; k < 2; k++) { c565(*(WORD *)(cb + 2 * k), t); if (0.299f * t[0] + 0.587f * t[1] + 0.114f * t[2] > mx) mx = 0.299f * t[0] + 0.587f * t[1] + 0.114f * t[2]; } }
            else { DWORD p = *(DWORD *)((BYTE *)b.pBits + y * b.Pitch + x * 4); float l = (0.299f * ((p >> 16) & 255) + 0.587f * ((p >> 8) & 255) + 0.114f * (p & 255)) / 255.f; if (l > mx) mx = l; }
        }
        if (mx < 0.05f) mx = 1.f;
        for (y = 0; y < rows; y++) for (x = 0; x < cols; x++) {
            if (!dxt) {
                DWORD *p = (DWORD *)((BYTE *)b.pBits + y * b.Pitch + x * 4), s = *p;
                float sh = (0.299f * ((s >> 16) & 255) + 0.587f * ((s >> 8) & 255) + 0.114f * (s & 255)) / 255.f / mx;
                *p = (s & 0xFF000000) | rb_colour(x + .5f, y + .5f, cx, cy, R, sh);
            } else {
                /* a 4x4 block: the wheel colours of its 16 texels -> two end colours along the block's main colour change */
                BYTE *cb = (BYTE *)b.pBits + y * b.Pitch + x * bs + co; WORD s0 = *(WORD *)cb, s1 = *(WORD *)(cb + 2); DWORD sidx = *(DWORD *)(cb + 4), idx = 0;
                float t0[3], t1[3], pal[4][3], col[16][3], lo[3], hi[3], best = -1; int i, j, k, transparentMode = dxt == 1 && s0 <= s1;
                c565(s0, t0); c565(s1, t1);
                for (k = 0; k < 3; k++) { pal[0][k] = t0[k]; pal[1][k] = t1[k]; pal[2][k] = transparentMode ? (t0[k] + t1[k]) / 2 : (2 * t0[k] + t1[k]) / 3; pal[3][k] = transparentMode ? 0 : (t0[k] + 2 * t1[k]) / 3; }
                for (i = 0; i < 16; i++) {
                    int si = (sidx >> (2 * i)) & 3; float sh = (0.299f * pal[si][0] + 0.587f * pal[si][1] + 0.114f * pal[si][2]) / mx; DWORD w = rb_colour(x * 4 + (i & 3) + .5f, y * 4 + (i >> 2) + .5f, cx, cy, R, sh);
                    col[i][0] = ((w >> 16) & 255) / 255.f; col[i][1] = ((w >> 8) & 255) / 255.f; col[i][2] = (w & 255) / 255.f;
                }
                for (i = 0; i < 16; i++) for (j = i + 1; j < 16; j++) {          /* the two most different texels = the ends */
                    float d = (col[i][0] - col[j][0]) * (col[i][0] - col[j][0]) + (col[i][1] - col[j][1]) * (col[i][1] - col[j][1]) + (col[i][2] - col[j][2]) * (col[i][2] - col[j][2]);
                    if (d > best) { best = d; memcpy(lo, col[i], 12); memcpy(hi, col[j], 12); }
                }
                {   WORD e0 = to565(((DWORD)(hi[0] * 255) << 16) | ((DWORD)(hi[1] * 255) << 8) | (DWORD)(hi[2] * 255)),
                         e1 = to565(((DWORD)(lo[0] * 255) << 16) | ((DWORD)(lo[1] * 255) << 8) | (DWORD)(lo[2] * 255));
                    float q0[3], q1[3], qp[4][3];
                    if (transparentMode) { if (e0 > e1) { WORD t = e0; e0 = e1; e1 = t; } }     /* keep 3 colours + transparent */
                    else { if (e0 < e1) { WORD t = e0; e0 = e1; e1 = t; } if (e0 == e1) { if (e0 < 0xFFFF) e0++; else e1--; } }
                    c565(e0, q0); c565(e1, q1);
                    for (k = 0; k < 3; k++) { qp[0][k] = q0[k]; qp[1][k] = q1[k]; qp[2][k] = transparentMode ? (q0[k] + q1[k]) / 2 : (2 * q0[k] + q1[k]) / 3; qp[3][k] = transparentMode ? 0 : (q0[k] + 2 * q1[k]) / 3; }
                    for (i = 0; i < 16; i++) {
                        int si = (sidx >> (2 * i)) & 3, bi = 0; float bd = 1e9f;
                        if (transparentMode && si == 3) { idx |= 3u << (2 * i); continue; }         /* transparent texel stays */
                        for (k = 0; k < (transparentMode ? 3 : 4); k++) { float dd = (qp[k][0] - col[i][0]) * (qp[k][0] - col[i][0]) + (qp[k][1] - col[i][1]) * (qp[k][1] - col[i][1]) + (qp[k][2] - col[i][2]) * (qp[k][2] - col[i][2]); if (dd < bd) { bd = dd; bi = k; } }
                        idx |= (DWORD)bi << (2 * i);
                    }
                    *(WORD *)cb = e0; *(WORD *)(cb + 2) = e1; *(DWORD *)(cb + 4) = idx;
                }
            }
        }
        IDirect3DTexture9_UnlockRect(nt, L);
    }
    c = (BYTE *)calloc(1, 0x7c + 0x60);
    if (!c) { IDirect3DTexture9_Release(nt); return NULL; }
    memcpy(c, src, 0x7c); memcpy(c + 0x7c, pi, 0x60);
    *(BYTE **)c = c + 0x7c; *(BYTE **)(c + 4) = NULL; *(BYTE **)(c + 8) = NULL;
    *(BYTE **)(c + 0x7c) = c + 0x7c; *(BYTE **)(c + 0x7c + 4) = c + 0x7c;
    *(IDirect3DTexture9 **)(c + 0x7c + 0x18) = nt;
    memset(c + 0xc, 0, 0x18); strcpy((char *)c + 0xc, "NFSU2CC_RAINBOW");
    *(DWORD *)(c + 0x24) = g_rbHash;
    clog("rainbow icon: made from %s (%ux%u, %s)", (char *)src + 0xc, d0.Width, d0.Height, dxt == 1 ? "DXT1" : dxt ? "DXT3/5" : "ARGB");
    return c;
}
static void rainbow_row_icon(void)            /* per frame: the CUSTOM row's list image shows the rainbow dot */
{
    BYTE *icon; DWORD hv;
    if (!g_nlItem || !g_gtiTramp || !nl_active()) return;
    icon = *(BYTE **)(g_nlItem + 0x10);
    if (!readable(icon, 0x5c) || (hv = *(DWORD *)(icon + 0x24)) == g_rbHash) return;
    if (!g_rbTI) {
        static DWORD triedHash;
        BYTE *src;
        if (hv == triedHash) return;
        triedHash = hv;
        __try { src = ((BYTE *(__cdecl *)(DWORD, int, int))0x4901d0)(hv, 0, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { src = NULL; }
        if (!src) { clog("rainbow icon: no texture for the row icon (hash %08lx)", hv); return; }
        g_rbTI = make_rainbow(src);
        if (!g_rbTI) return;
    }
    *(DWORD *)(icon + 0x24) = g_rbHash;
    *(DWORD *)(icon + 0x1c) |= 0x400000;
}
static int dot_due(void)
{
    return g_enabled && !g_shopMode && g_inShop && *(int *)FLOW == 3 && g_nav && shop_nav(g_nav) && GetTickCount() - g_shopEnterT > 350 && !nl_active();
}
static void rainbow_dot(IDirect3DDevice9 *d, int W, int H)
{
    float s, ox, oy, sx, sy, wp, tw, gap, x0, cx, cy, r, rr; V v[26]; int i, pass;
    if (!dot_due()) return;
    if (g_fontDev != d) { if (g_font) IDirect3DTexture9_Release(g_font); g_font = NULL; make_font(d); }
    wp = text_w("Preset colors", 1.f); if (wp <= 0) return;
    tw = 63.f * text_w(S_CC_BTN, 1.f) / wp; gap = 63.f * text_w("   ", 1.f) / wp;
    fe_map(W, H, &s, &ox, &oy, &sx, &sy);
    x0 = 543.f - tw / 2;
    r = gap * 0.4f; if (r > 4.5f) r = 4.5f; if (r < 3.f) r = 3.f;
    cx = ox + (x0 + gap / 2) * sx; cy = oy + 442.f * sy;       /* the game's label is stretched with the menu, the dot is not */
    solid(d);
    for (pass = 0; pass < 2; pass++) {          /* thin dark rim, then the hue wheel with a white centre */
        rr = (pass ? r : r + 0.6f) * s;
        v[0].x = cx; v[0].y = cy; v[0].z = 0; v[0].rhw = 1; v[0].u = v[0].v = 0; v[0].c = pass ? 0xFFFFFFFF : 0xC0000000;
        for (i = 0; i <= 24; i++) {
            float a = i * 6.2831853f / 24;
            v[i + 1].x = cx + cosf(a) * rr; v[i + 1].y = cy - sinf(a) * rr; v[i + 1].z = 0; v[i + 1].rhw = 1; v[i + 1].u = v[i + 1].v = 0;
            v[i + 1].c = pass ? argb(hsv_rgb((float)(i % 24) * 15.f, 1, 1), 255) : 0xC0000000;
        }
        IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLEFAN, 24, v, sizeof(V));
    }
}
static void __cdecl draw_cb(IDirect3DDevice9 *d, int W, int H)
{
    LONG w;
    g_cDraw++;
    input_setup();
    key_repeat();
    purge_refresh();
    pic_install_watch();
    decal_sweep();
    watch_fe();
    hud_fix();
    hudx_apply();
    recomposite_due();
    while ((w = g_qr) != g_qw) { DWORD ev = g_q[w % QN]; InterlockedIncrement(&g_qr); handle_key(ev); }
    if (g_dirty && GetTickCount() - g_dirtyAt > 500) { g_dirty = 0; EnterCriticalSection(&g_cs); save_store(); LeaveCriticalSection(&g_cs); }
    {
        int in = *(int *)FLOW == 3 && g_nav && shop_nav(g_nav) && *(BYTE *)(g_nav + 0x6c);
        if (!in && g_inShop && g_shopMode) { g_shopMode = 0; g_open = 0; clog("shop: left the screen, picker closed"); }
        if (in && !g_inShop) { g_shopEnterT = GetTickCount(); g_autoDone = 0; InterlockedIncrement(&g_ver); }
        if (!in && g_inShop) { InterlockedIncrement(&g_ver); g_paintScr = NULL; }
        g_inShop = in;
        if (in) auto_open();
    }
    nl_update();
    rainbow_row_icon();
    vpic_labels();
#ifndef NFSU2CC_SHIP
    publish_yaw();
#endif
    rainbow_dot(d, W, H);
    nl_draw(d, W, H);
    if (g_decalLblPart && GetTickCount() - g_decalToast > 1500) {   /* "COLOUR INSTALLED" -> the decal's own name again */
        BYTE *pp = g_decalLblPart; g_decalLblPart = NULL;
        __try { char *nm = ((char *(__stdcall *)(BYTE *))0x50e530)(pp); if (nm && g_decalLblPk[0]) ((void (__cdecl *)(const char *, DWORD, const char *))0x537b80)(g_decalLblPk, 0xd28b9316, nm); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (0) {          /* (the drawn decal toast is replaced by the game's own name label) */
        float s2, ox2, oy2, x0, y0, w, h, ts; const char *m = "COLOUR INSTALLED";
        if ((float)W / H >= 4.f / 3.f) { s2 = H / 480.f; ox2 = (W - 640.f * s2) / 2; oy2 = 0; } else { s2 = W / 640.f; ox2 = 0; oy2 = (H - 480.f * s2) / 2; }
        if (g_fontDev != d) { if (g_font) IDirect3DTexture9_Release(g_font); g_font = NULL; make_font(d); }
        x0 = ox2 + 196 * s2; y0 = oy2 + 398 * s2; w = 186 * s2; h = 20 * s2; ts = 13.f * s2 / g_fontH;
        quad(d, x0, y0, w, h, 0xF0181C14); text(d, m, x0 + (w - text_w(m, ts)) / 2, y0 + 1.5f * s2, ts, GAME_GREEN);
    }
    ns_panel(d, W, H);
    if (!g_open) return;
    if (g_pRide && !readable(g_pRide, 0x590 + 170 * 4)) { g_open = 0; g_shopMode = 0; return; }
    track_type();
    if (g_fontDev != d) { if (g_font) IDirect3DTexture9_Release(g_font); g_font = NULL; make_font(d); }
    apply_mouse();
    if (g_shopMode) draw_shop_picker(d, W, H); else draw_picker(d, W, H);
}

/* ---------------- setup ---------------- */
typedef void (__cdecl *HDT_DrawCb)(IDirect3DDevice9 *, int, int);
typedef void (__cdecl *PFN_AddCb)(HDT_DrawCb);
/* Drawing the picker. With NFSU2_HDToggle.asi (that has HDToggle_AddDrawCallback) the plugin draws through it; on its own
   (another player's game) it hooks IDirect3DDevice9::Present of the game's device ([0x870974]) and sets up the 2D state
   itself. Every frame draw_cb also runs the per-frame work (keys, shop buttons, saving). */
#define GAME_DEVICE 0x870974
typedef HRESULT (__stdcall *PFN_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static PFN_Present o_Present; static void **g_devVt;
static void own_draw(IDirect3DDevice9 *d)
{
    IDirect3DStateBlock9 *sb = NULL; IDirect3DSurface9 *bb = NULL, *oldRT = NULL, *oldDS = NULL; D3DSURFACE_DESC bd;
    if (IDirect3DDevice9_TestCooperativeLevel(d) != D3D_OK) return;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface9_GetDesc(bb, &bd);
    if (!g_open && !dot_due() && !ns_due()) { draw_cb(d, (int)bd.Width, (int)bd.Height); IDirect3DSurface9_Release(bb); return; }   /* per-frame work only */
    if (FAILED(IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &sb))) { IDirect3DSurface9_Release(bb); return; }
    IDirect3DDevice9_GetRenderTarget(d, 0, &oldRT);
    IDirect3DDevice9_GetDepthStencilSurface(d, &oldDS);
    IDirect3DDevice9_SetRenderTarget(d, 0, bb);
    IDirect3DDevice9_SetDepthStencilSurface(d, NULL);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(d))) {
        IDirect3DDevice9_SetVertexShader(d, NULL);
        IDirect3DDevice9_SetPixelShader(d, NULL);
        IDirect3DDevice9_SetFVF(d, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        IDirect3DDevice9_SetTexture(d, 0, NULL); IDirect3DDevice9_SetTexture(d, 1, NULL);
        IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_ZWRITEENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetRenderState(d, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice9_SetRenderState(d, D3DRS_BLENDOP, D3DBLENDOP_ADD);
        IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_FOGENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_STENCILENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE, 0xF);
        IDirect3DDevice9_SetRenderState(d, D3DRS_SRGBWRITEENABLE, FALSE);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_TEXCOORDINDEX, 0);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        IDirect3DDevice9_SetTextureStageState(d, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        IDirect3DDevice9_SetTextureStageState(d, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_SRGBTEXTURE, FALSE);
        draw_cb(d, (int)bd.Width, (int)bd.Height);
        IDirect3DDevice9_EndScene(d);
    }
    IDirect3DDevice9_SetRenderTarget(d, 0, oldRT ? oldRT : bb);
    IDirect3DDevice9_SetDepthStencilSurface(d, oldDS);
    IDirect3DStateBlock9_Apply(sb);
    IDirect3DStateBlock9_Release(sb);
    if (oldRT) IDirect3DSurface9_Release(oldRT);
    if (oldDS) IDirect3DSurface9_Release(oldDS);
    IDirect3DSurface9_Release(bb);
}
static HRESULT __stdcall h_Present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
    __try { own_draw(d); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 3) clog("picker draw: exception"); }
    return o_Present(d, a, b, w, r);
}
static int g_ownDraw;
static DWORD WINAPI late_init(LPVOID u)
{
    int i; (void)u;
    for (i = 0; i < 30 && !g_ownDraw; i++) {                               /* HDToggle (if installed) is loaded together with us */
        HMODULE h = GetModuleHandleA("NFSU2_HDToggle.asi");
        PFN_AddCb add = h ? (PFN_AddCb)GetProcAddress(h, "HDToggle_AddDrawCallback") : NULL;
        if (add) { add(draw_cb); clog("picker: drawing through NFSU2_HDToggle.asi"); return 0; }
        Sleep(100);
    }
    for (i = 0; i < 6000; i++) {                             /* own Present hook once the game made its device */
        IDirect3DDevice9 *d = *(IDirect3DDevice9 **)GAME_DEVICE;
        if (d && readable(d, 4)) {
            void **vt = *(void ***)d; DWORD old;
            if (readable(vt, 18 * 4) && VirtualProtect(&vt[17], 4, PAGE_EXECUTE_READWRITE, &old)) {
                o_Present = (PFN_Present)vt[17]; vt[17] = (void *)h_Present; VirtualProtect(&vt[17], 4, old, &old);
                g_devVt = vt; clog("picker: drawing on its own (Present hook, device %p)", d);
                return 0;
            }
        }
        Sleep(100);
    }
    clog("picker: no game device found - picker off (saved colours still work)");
    return 0;
}
static int parse_key(const char *s, int def)
{
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') { int n = atoi(s + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s, NULL, 16);
    return def;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
{
    char ini[MAX_PATH], k[64], *s;
    static const BYTE eAttr[] = { 0x66, 0x8B, 0x41, 0x0A, 0x66, 0x3D, 0xFF, 0xFF };
    static const BYTE eComp[] = { 0x81, 0xEC, 0xC4, 0x03, 0x00, 0x00 };
    static const BYTE eNeon[] = { 0x83, 0xEC, 0x0C, 0x56, 0x8B, 0xF1 };
    (void)r;
    if (why == DLL_PROCESS_DETACH && g_dirty) { save_store(); return TRUE; }
    if (why != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h); g_self = h;
    InitializeCriticalSection(&g_cs);
    GetModuleFileNameA(h, g_dir, MAX_PATH); s = strrchr(g_dir, '\\'); if (s) *s = 0;
    sprintf(ini, "%s\\NFSU2_CustomColor.ini", g_dir);
    sprintf(g_logp, "%s\\NFSU2_CustomColor.log", g_dir);
    g_enabled = GetPrivateProfileIntA("CustomColor", "Enabled", 1, ini);
#ifdef NFSU2CC_SHIP
    g_log = GetPrivateProfileIntA("CustomColor", "Log", 0, ini);
#else
    g_log = GetPrivateProfileIntA("CustomColor", "Log", 1, ini);
#endif
    GetPrivateProfileStringA("CustomColor", "PickerKey", "F4", k, sizeof k, ini); g_key = parse_key(k, VK_F4);
    g_kindOn[K_PAINT] = GetPrivateProfileIntA("CustomColor", "Paint", 1, ini);
    g_kindOn[K_NEON] = GetPrivateProfileIntA("CustomColor", "Neon", 1, ini);
    g_kindOn[K_LIGHT] = GetPrivateProfileIntA("CustomColor", "Headlights", 1, ini);
    g_kindOn[K_HUD] = GetPrivateProfileIntA("CustomColor", "Hud", 1, ini);
    g_kindOn[K_SHINE] = GetPrivateProfileIntA("CustomColor", "Shine", 1, ini);
    g_kindOn[K_PURGE] = GetPrivateProfileIntA("CustomColor", "Purge", 1, ini);
#ifdef NFSU2CC_SHIP
    g_kindOn[K_NEON] = g_kindOn[K_LIGHT] = g_kindOn[K_HUD] = g_kindOn[K_PURGE] = 0;   /* ship build: paint, vinyls, decals, shine only */
#endif
    g_ownDraw = GetPrivateProfileIntA("CustomColor", "OwnDraw", 0, ini);
    g_menuLayout = GetPrivateProfileIntA("CustomColor", "MenuLayout", 0, ini);
    { int mb = GetPrivateProfileIntA("CustomColor", "VinylTintMemoryMB", 160, ini); if (mb >= 16 && mb <= 1024) g_vtBudget = (size_t)mb << 20;
      mb = GetPrivateProfileIntA("CustomColor", "DecalTintMemoryMB", 192, ini); if (mb >= 16 && mb <= 1024) g_dcBudget = (size_t)mb << 20; }
    g_skin2Shine = GetPrivateProfileIntA("CustomColor", "Skin2Shine", 0, ini);
    g_rimLodFix = GetPrivateProfileIntA("CustomColor", "RimLodFix", 0, ini);
    { char b3[32]; GetPrivateProfileStringA("CustomColor", "OrbitDistance", "0", b3, sizeof b3, ini); g_orbIniR = (float)atof(b3);
      GetPrivateProfileStringA("CustomColor", "OrbitHeight", "0", b3, sizeof b3, ini); g_orbIniH = (float)atof(b3); }
    { char b2[32]; GetPrivateProfileStringA("CustomColor", "ShineStrength", "1.6", b2, sizeof b2, ini); g_shineMax = (float)atof(b2); if (g_shineMax < 1.f || g_shineMax > 8.f) g_shineMax = 1.6f; }   /* 0 = 2nd skin slot (trunk box, trim) gets a plain copy, no shine */
    { char b[32]; GetPrivateProfileStringA("CustomColor", "PartBrightness", "1.35", b, sizeof b, ini); g_partBright = (float)atof(b); if (g_partBright < 0.3f || g_partBright > 4.f) g_partBright = 1.35f;
      GetPrivateProfileStringA("CustomColor", "RimBrightness", "2.0", b, sizeof b, ini); g_rimBright = (float)atof(b); if (g_rimBright < 0.3f || g_rimBright > 4.f) g_rimBright = 2.0f; }     /* 1 = never use NFSU2_HDToggle for drawing */
    GetPrivateProfileStringA("CustomColor", "File", "NFSU2_CustomColor.txt", k, sizeof k, ini);
    if (strchr(k, ':')) strcpy(g_store, k); else sprintf(g_store, "%s\\%s", g_dir, k);
    { FILE *f = fopen(g_logp, "w"); if (f) fclose(f); }
    H_RED = bin_hash("RED"); H_GREEN = bin_hash("GREEN"); H_BLUE = bin_hash("BLUE"); H_GLOSS = bin_hash("GLOSS");
    clog("NFSU2_CustomColor v1 loaded: enabled %d, picker key 0x%02x, paint %d neon %d headlights %d hud %d, file %s",
         g_enabled, g_key, g_kindOn[K_PAINT], g_kindOn[K_NEON], g_kindOn[K_LIGHT], g_kindOn[K_HUD], g_store);
    if (!g_enabled) return TRUE;
    load_store();
    sprintf(g_unlockFile, "%s\\NFSU2_CustomColor_unlocks.txt", g_dir);
    g_neonUnlocked = GetPrivateProfileIntA("CustomColor", "NeonCustomAlwaysUnlocked", 0, ini) ? 2 : 0;
    if (!g_neonUnlocked) { FILE *f = fopen(g_unlockFile, "r"); char ln[128];
        if (f) { while (fgets(ln, sizeof ln, f)) if (!strncmp(ln, "NeonCustom=1", 12)) g_neonUnlocked = 1; fclose(f); } }
    H_ENDING = bin_hash("EndingNis01");
    sprintf(g_preFile, "%s\\NFSU2_CustomColor_presets.txt", g_dir); load_pre();
    g_fLo = (DWORD)&g_fakes[0]; g_fHi = (DWORD)&g_fakes[NSLOT];
    make_hook(A_ATTR, eAttr, sizeof eAttr, h_attr, (void **)&o_attr);
    if (!o_attr) { clog("attribute hook failed - plugin off"); g_enabled = 0; return TRUE; }
    make_hook(A_COMPOSITE, eComp, sizeof eComp, h_comp, (void **)&o_comp);
    g_decalOn = GetPrivateProfileIntA("CustomColor", "DecalColours", 1, ini);
    if (g_decalOn) {
        static const BYTE eNl2[] = { 0x55, 0x8B, 0xEC, 0x51, 0x57, 0x8B, 0xF9 };
        make_hook(0x54e6e0, eNl2, sizeof eNl2, h_navlayout, &g_navLayTramp);
        { static const BYTE eDm[] = { 0x55, 0x8B, 0xEC, 0x56, 0x57 }; make_hook(0x56b3b0, eDm, sizeof eDm, h_decalmsg, &g_dmTramp); }
        clog("decal screen buttons: %s", g_navLayTramp ? "ok" : "FAILED");
    }
    g_nlOn = GetPrivateProfileIntA("CustomColor", "NeonCustomRow", 1, ini);
#ifdef NFSU2CC_SHIP
    g_nlOn = 0;
#endif
    if (g_nlOn && g_kindOn[K_NEON]) {
        static const BYTE eNl[] = { 0x6A, 0xFF, 0x68, 0xC8, 0x75, 0x77, 0x00 };
        make_hook(A_NEONLIST, eNl, sizeof eNl, h_neonlist, &g_nlTramp);
        clog("neon list CUSTOM row: %s", g_nlTramp ? "ok" : "FAILED");
    }
    if (g_nlOn && g_kindOn[K_PURGE]) {
        static const BYTE ePl[] = { 0x6A, 0xFF, 0x68, 0xC8, 0x75, 0x77, 0x00 };
        make_hook(0x5606e0, ePl, sizeof ePl, h_purgelist, &g_plTramp);
        clog("purge list CUSTOM row: %s", g_plTramp ? "ok" : "FAILED");
    }
    if (g_nlOn) {
        static const BYTE eNm[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 }, eIm[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 };
        if (g_kindOn[K_NEON]) make_hook(0x56a300, eNm, sizeof eNm, h_neonmsg, &g_nmTramp[0]);
        { static const BYTE ePn[] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x85, 0xF6 }; make_hook(0x50e530, ePn, sizeof ePn, h_partname, &g_pnTramp); }
        { static const BYTE eGt[] = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14 }; g_rbHash = bin_hash("NFSU2CC_RAINBOW");
          make_hook(0x4901d0, eGt, sizeof eGt, h_gettex, &g_gtiTramp); clog("rainbow row icon: %s", g_gtiTramp ? "ok" : "FAILED"); }
        if (g_kindOn[K_PURGE]) make_hook(0x568d30, eIm, sizeof eIm, h_icemsg, &g_nmTramp[1]);
        clog("CUSTOM row install: neon %s, purge %s", g_nmTramp[0] ? "ok" : "off", g_nmTramp[1] ? "ok" : "off");
    }
    g_vTint = GetPrivateProfileIntA("CustomColor", "VinylTintPictures", 1, ini);
    if (g_vTint) {    /* call 0x611970 at 0x61dd8b -> h_vcache (same arguments, cdecl) */
        BYTE *c = (BYTE *)A_VCACHECALL; DWORD old;
        if (c[0] == 0xE8 && *(DWORD *)(c + 1) == A_VCACHE - (A_VCACHECALL + 5)) {
            VirtualProtect(c, 5, PAGE_EXECUTE_READWRITE, &old); *(DWORD *)(c + 1) = (DWORD)h_vcache - (A_VCACHECALL + 5); VirtualProtect(c, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), c, 5);
            clog("picture vinyls: take the vinyl colour (ini VinylTintPictures=1)");
        } else clog("picture vinyls: call at 0x61dd8b not as expected - off");
    }
    make_hook(A_NEONDRAW, eNeon, sizeof eNeon, h_neon, (void **)&g_neonTramp);
    if (g_kindOn[K_PAINT]) hook_matreads();
    if (g_kindOn[K_HUD]) {
        static const BYTE eHud[] = { 0x8B, 0x4E, 0x50, 0x57, 0x51 };
        make_hook(A_HUDAPPLY, eHud, sizeof eHud, h_hudapply, &o_hudapply);
        clog("in-game gauge colours: %s", o_hudapply ? "ok" : "FAILED");
    }
    if (g_kindOn[K_PURGE]) {
        static const BYTE eBr[] = { 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x0D, 0x84, 0xB8, 0x82, 0x00 };
        static const BYTE ePu[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
        make_hook(A_BROWSERNAV, eBr, sizeof eBr, h_browsernav, &o_browsernav);
        clog("parts browser (purge button): %s", o_browsernav ? "ok" : "FAILED");
        make_hook(A_PURGE, ePu, sizeof ePu, h_purge, &o_purge);
        clog("nitrous purge colours: %s", o_purge ? "ok" : "FAILED");
    }
#ifndef NFSU2CC_SHIP
    {
        static const BYTE eNis[] = { 0x55, 0x8B, 0xEC, 0x51, 0xA1, 0x98, 0xB8, 0x82, 0x00 };
        make_hook(A_NISSTART, eNis, sizeof eNis, h_nis, &o_nis);
        clog("custom neon: %s (%s), career-end watch %s", g_neonUnlocked ? "unlocked" : "locked until the final career race is won",
             g_neonUnlocked == 2 ? "ini NeonCustomAlwaysUnlocked=1" : g_neonUnlocked ? "career finished" : "no reward yet", o_nis ? "ok" : "FAILED");
    }
#endif
    g_turnOn = GetPrivateProfileIntA("CustomColor", "TurnKeys", 1, ini);
    if (g_turnOn) {
        static const BYTE eFe[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
        make_hook(A_FECARS, eFe, sizeof eFe, h_fecars, &o_fecars);
        clog("turntable keys K / L: %s", o_fecars ? "ok" : "FAILED");
        g_driveOrbit = GetPrivateProfileIntA("CustomColor", "DriveOrbit", 0, ini);
#ifdef NFSU2CC_SHIP
        g_driveOrbit = 0;
#endif
        if (g_driveOrbit) {   static const BYTE eCam[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
            make_hook(A_SETCAM, eCam, sizeof eCam, h_setcam, &o_setcam);
            clog("driving camera K / L: %s", o_setcam ? "ok" : "FAILED"); }
        else clog("driving camera K / L: off (ini DriveOrbit=0)");
        if (!o_fecars) g_turnOn = 0;
    }
    clog("hooks: attributes ok, composite %s, neon draw %s", o_comp ? "ok" : "FAILED", g_neonTramp ? "ok" : "FAILED");
    g_nsOn = GetPrivateProfileIntA("CustomColor", "NeonSettings", 1, ini);   /* 0 = no Neon settings panel in the neon shop */
#ifdef NFSU2CC_SHIP
    g_nsOn = 0;
#endif
    H_CC_BTN = bin_hash("NFSU2CC_CUSTOM_COLOR"); H_CC_PRESET = bin_hash("NFSU2CC_PRESET_COLORS");
    if (GetPrivateProfileIntA("CustomColor", "ShopButton", 1, ini)) {
        static const BYTE eLabel[] = { 0x8B, 0x4C, 0x24, 0x04, 0xF6, 0xC1, 0x20 };
        static const BYTE eClick[] = { 0x8B, 0x46, 0x1C, 0xA8, 0x02 };
        static const BYTE eLang[]  = { 0x53, 0x8B, 0x1D, 0xD8, 0x83, 0x83, 0x00 };
        static const BYTE eSet[]   = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8, 0x61 };
        GetPrivateProfileStringA("CustomColor", "ShopScreens", g_shopPk, g_shopPk, sizeof g_shopPk, ini);
#ifndef NFSU2CC_SHIP
        if (!strstr(g_shopPk, "UI_NeonPartsOverlay.fng") && strlen(g_shopPk) < sizeof g_shopPk - 26) strcat(g_shopPk, ",UI_NeonPartsOverlay.fng");
        if (!strstr(g_shopPk, "UI_CustomHUDOverlay.fng") && strlen(g_shopPk) < sizeof g_shopPk - 26) strcat(g_shopPk, ",UI_CustomHUDOverlay.fng");
#endif
        if (!strstr(g_shopPk, "UI_GenericParts_Browser.fng") && strlen(g_shopPk) < sizeof g_shopPk - 30) strcat(g_shopPk, ",UI_GenericParts_Browser.fng");
        make_hook(A_LANG, eLang, sizeof eLang, h_lang, &o_lang);
        make_hook(A_SETPART, eSet, sizeof eSet, h_setpart, &o_setpart);
        if (o_lang) make_hook(A_NAVLABEL, eLabel, sizeof eLabel, h_navlabel, (void **)&o_navlabel);
        if (o_lang && o_navlabel) make_hook(A_NAVCLICK, eClick, sizeof eClick, h_navclick, &g_clickTramp);
        {
            static const BYTE eMsg[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1 };
            make_hook(A_PAINTMSG, eMsg, sizeof eMsg, h_paintmsg, &o_paintmsg);
            clog("paint screen dot blocking %s", o_paintmsg ? "ok" : "FAILED");
        }
        clog("shop button hooks: text %s, labels %s, clicks %s, part changes %s (screens %s)", o_lang ? "ok" : "FAILED",
             o_navlabel ? "ok" : "FAILED", g_clickTramp ? "ok" : "FAILED", o_setpart ? "ok" : "FAILED", g_shopPk);
    }
    {
        static const BYTE eMouse[] = { 0x83, 0xEC, 0x14, 0x56, 0x8B, 0xF1 };
        make_hook(A_MOUSEPOLL, eMouse, sizeof eMouse, h_mpoll, (void **)&o_mpoll);
        g_pickDik = MapVirtualKeyA(g_key, 0);
        clog("input: mouse %s, picker key scan code 0x%02lx", o_mpoll ? "ok" : "FAILED", g_pickDik);
    }
    CreateThread(NULL, 0, late_init, NULL, 0, NULL);
    return TRUE;
}
