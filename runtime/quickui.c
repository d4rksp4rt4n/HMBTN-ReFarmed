/* Shorter waits: windows and fades (quickui), loading (fastload), typing (textspeed) and
 * cutscenes (skipscenes). Hooks emitted by tools/recomp.py. */
#include "psx.h"

/* The message/help window (the box at the bottom of the rucksack, the toolbox, the bookshelf,
 * ...) grows open and shrinks closed over 6 + steps game ticks, counted in the word at
 * `counter`; once it reaches 6 + steps the window is drawn whole (or gone) and the caller
 * goes on. Moving the counter there makes the window open and close at once, but only from
 * its second tick: callers that see the window done on the very first one skip a step of
 * their own (closing the help box to show an item's description then left the description
 * stuck, with no way to scroll or close it).
 * The item description (Triangle in the rucksack / toolbox) also opens in two stages: its
 * first stage (the call returning to 80022A84) runs the animation while the text is made
 * ready and, if the window is not open yet, hands over to the second (80022B20), which marks
 * the message window as open (8006C904 = 4) once the animation ends. That first stage is
 * left alone, so the second one always runs; it opens the window at once. */
#define DESCRIPTION_OPENING 0x80022A84u

void quick_window(uint32_t ra, uint32_t counter, uint32_t steps) {
    if (!g_cfg.quickui || ra == DESCRIPTION_OPENING || MEM_R32(counter) == 0) return;
    int32_t end = (int32_t)steps + 6;
    if ((int32_t)MEM_R32(counter) < end) MEM_W32(counter, (uint32_t)end);
}

/* Fades to and from black (doors, the map, menus, going to bed, ...) last the given number of
 * game ticks, and the game waits for them to finish: a quarter of it still shows the fade. */
uint32_t quick_fade(uint32_t ticks) {
    int16_t n = (int16_t)ticks;
    if (!g_cfg.quickui || n <= 2) return ticks;
    n = (int16_t)((n + 3) / 4);
    return (uint32_t)(int32_t)(n < 2 ? 2 : n);
}

/* "Now Loading" (the dog in the middle of a black screen while an area is read from the disc):
 * the game sets gp+88 (8005E2CC) while it is up, and its draw callback (8001B118) only draws
 * it then. The wait is the disc and the data setup, so fastload runs those frames at full
 * speed, like holding Tab; so are the black pauses between places. */
bool quick_loading(bool black) {
    if (!g_cfg.fastload) return false;
    if (ram_r32(0x8005E2CCu) != 0) return true;
    /* After a fade to black (doors, going to bed, between the intro screens) the fade kind at
     * gp+32 (8005E294) is 0 until the next fade from black starts, while the game loads the
     * next place. Only frames that really are black count, in case something is drawn then. */
    return black && (int16_t)MEM_R16(0x8005E294u) == 0;
}

/* Typed text (dialogue, the TV, signs, ...): gp+464 (8005E444) counts the characters of the
 * text shown so far; the printer adds one each time its timer runs out and handles every
 * control code itself as the count reaches it (line ends, waits for the button, names,
 * choices, ...). textspeed adds more characters at that moment, but only plain letters and
 * spaces: the count never passes a control code, so the printer still meets each one as in
 * the original and only the typing is faster. */
#define TEXT_SHOWN 0x8005E444u

static bool plain_char(uint16_t ch) {
    return ch < 0x0E00u || ch == 0x0FFDu;   /* letters, digits, punctuation; 0FFD = space */
}

void quick_text(uint32_t text) {
    if (g_cfg.textspeed <= 1) return;
    uint32_t n = MEM_R16(TEXT_SHOWN);
    if (n == 0 || !plain_char((uint16_t)MEM_R16(text + (n - 1) * 2u))) return;
    for (int k = 1; k < g_cfg.textspeed && n < 4096; k++, n++)
        if (!plain_char((uint16_t)MEM_R16(text + n * 2u))) break;
    MEM_W16(TEXT_SHOWN, n);
}

/* Cutscenes: the event scripts lock the player (bit 23 of the player state word, 80071A6C,
 * set and cleared by the script command at 800E63C8) for as long as they run. Holding START
 * then runs the game at full speed; the game never sees that START (it opens the farm summary
 * in normal play), not even if the scene ends while it is still held. Whenever a message
 * window is up (8006C904, set while the dialogue box is open) the scene runs at normal speed
 * and waits for the player, since some dialogue asks for a choice: once the box closes,
 * holding START goes on skipping. */
#define PLAYER_STATE   0x80071A6Cu
#define PLAYER_LOCKED  0x00800000u
#define DIALOGUE_OPEN  0x8006C904u

static bool skipping, start_swallowed;

uint16_t quick_skip(uint16_t pad) {
    bool start = (pad & PAD_START) != 0;
    bool scene = g_cfg.skipscenes && (MEM_R32(PLAYER_STATE) & PLAYER_LOCKED) != 0;
    if (!start) start_swallowed = false;
    else if (scene) start_swallowed = true;
    if (start_swallowed) pad &= (uint16_t)~PAD_START;
    skipping = scene && start && MEM_R8(DIALOGUE_OPEN) == 0;
    return pad;
}

bool quick_skipping(void) {
    return skipping;
}
