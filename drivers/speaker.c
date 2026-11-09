#include "../include/io.h"
#include "../include/types.h"

/* PC Speaker via port 0x61 and PIT channel 2 */

void speaker_off(void);   /* forward declaration */

static void speaker_on(uint32_t freq){
    if(freq==0){speaker_off();return;}
    uint32_t div=1193180/freq;
    outb(0x43,0xB6);               /* channel 2, square wave */
    outb(0x42,(uint8_t)(div&0xFF));
    outb(0x42,(uint8_t)(div>>8));
    outb(0x61,inb(0x61)|0x03);     /* enable speaker gate */
}

void speaker_off(void){
    outb(0x61,inb(0x61)&~0x03);
}

static void busy_wait(volatile uint32_t n){ while(n--) __asm__ volatile("nop"); }

void speaker_beep(uint32_t freq, uint32_t ms){
    speaker_on(freq);
    busy_wait(ms*40000);
    speaker_off();
}

/* Note frequencies */
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_REST 0

typedef struct { uint32_t freq; uint32_t ms; } note_t;

/* Boot jingle - TechHaven Studios fanfare */
static const note_t boot_jingle[]={
    {NOTE_E4, 80},{NOTE_REST,20},{NOTE_G4,80},{NOTE_REST,20},
    {NOTE_C5, 120},{NOTE_REST,20},{NOTE_E5,80},{NOTE_REST,20},
    {NOTE_G5, 200},{NOTE_REST,40},
    {NOTE_E5, 80},{NOTE_REST,20},{NOTE_C5,80},{NOTE_REST,20},
    {NOTE_G4, 80},{NOTE_REST,20},{NOTE_A4,80},{NOTE_REST,20},
    {NOTE_B4, 300},{NOTE_REST,80},
    {NOTE_C5, 80},{NOTE_D5,80},{NOTE_E5,80},{NOTE_REST,20},
    {NOTE_G5, 400},
    {0,0}
};

/* Startup chime - short version */
static const note_t startup_chime[]={
    {NOTE_C5,80},{NOTE_E5,80},{NOTE_G5,80},{NOTE_C5*2,150},
    {0,0}
};

/* Error beep */
static const note_t error_beep[]={
    {200,150},{NOTE_REST,50},{200,150},
    {0,0}
};

/* Game over */
static const note_t gameover[]={
    {NOTE_G4,100},{NOTE_E4,100},{NOTE_C4,100},{NOTE_A4-50,400},
    {0,0}
};

/* Level up */
static const note_t levelup[]={
    {NOTE_C4,60},{NOTE_E4,60},{NOTE_G4,60},{NOTE_C5,120},
    {0,0}
};

static void play_melody(const note_t *melody){
    for(int i=0; melody[i].freq!=0||melody[i].ms!=0; i++){
        if(melody[i].freq==NOTE_REST||melody[i].freq==0)
            busy_wait(melody[i].ms*40000);
        else
            speaker_beep(melody[i].freq, melody[i].ms);
    }
}

void music_boot_jingle(void) { play_melody(boot_jingle); }
void music_startup(void)     { play_melody(startup_chime); }
void music_error(void)       { play_melody(error_beep); }
void music_gameover(void)    { play_melody(gameover); }
void music_levelup(void)     { play_melody(levelup); }
