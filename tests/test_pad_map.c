/* MT_PAD_MAP / MT_KEY_MAP parsing, mouse sources and mouse look (src/runtime_pad.c). */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"

int mtgpu_overlay_captures_input(void) { return 0; }
int mtgpu_vibration_percent(void) { return 100; }
static int pause_keys;
void mtgpu_mouse_pause_key(void) { ++pause_keys; }
static int look_style;
int mtgpu_mouse_aim_style(void) { return look_style; }
static int look_sensitivity=100;
int mtgpu_mouse_aim_sensitivity(void) { return look_sensitivity; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* PS4 buttons a host source presses in `map`. */
static uint32_t pressed_by(const ButtonMap *map, int source) {
    uint32_t ps=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].source==source) ps|=map->entries[i].ps;
    return ps;
}

int main(void) {
    /* GODZILLA's defaults: Q and E turn (L1, R1), the mouse buttons attack. */
    assert(pressed_by(&key_map,SDL_SCANCODE_Q)==BTN_L1 && pressed_by(&key_map,SDL_SCANCODE_E)==BTN_R1);
    assert(pressed_by(&key_map,MOUSE_LEFT)==BTN_SQUARE && pressed_by(&key_map,MOUSE_RIGHT)==BTN_TRIANGLE);
    assert(pressed_by(&key_map,SDL_SCANCODE_W)==MOVE_UP && pressed_by(&key_map,SDL_SCANCODE_SPACE)==BTN_CROSS);

    ButtonMap directions=key_map;
    assert(apply_button_map(&directions,"move_up=T,move_down=G,move_left=F,move_right=H,"
                                       "look_up=Keypad 8,look_down=Keypad 2,"
                                       "look_left=Keypad 4,look_right=None",true)==8);

    ButtonMap keys=key_map;
    assert(apply_button_map(&keys,NULL,true)==0 && apply_button_map(&keys,"",true)==0);
    assert(keys.count==key_map.count);

    /* A mapped button loses its default sources: V presses Cross, Space presses Triangle. */
    assert(apply_button_map(&keys," cross = V , triangle=Space",true)==2);
    assert(pressed_by(&keys,SDL_SCANCODE_V)==BTN_CROSS);
    assert(pressed_by(&keys,SDL_SCANCODE_SPACE)==BTN_TRIANGLE);
    assert(pressed_by(&keys,MOUSE_RIGHT)==0 && pressed_by(&keys,MOUSE_LEFT)==BTN_SQUARE);

    /* Names with spaces, any case; the last entry for a button wins. */
    ButtonMap again=key_map;
    assert(apply_button_map(&again,"CIRCLE=left shift,circle=Left Ctrl",true)==2);
    assert(pressed_by(&again,SDL_SCANCODE_LCTRL)==BTN_CIRCLE);
    assert(pressed_by(&again,SDL_SCANCODE_LSHIFT)==0);

    /* Bad entries are skipped and leave the defaults alone. */
    ButtonMap bad=key_map;
    assert(apply_button_map(&bad,"bogus=Q,cross=NotAKey,circle,=Q,square=,,",true)==0);
    assert(bad.count==key_map.count && pressed_by(&bad,SDL_SCANCODE_SPACE)==BTN_CROSS);
    /* The actions of other games are no buttons here. */
    assert(apply_button_map(&bad,"rage=V,dodge=Left Alt,weapon_next=Wheel Up",true)==0);

    /* Gamepad: Xbox layout swap; 'touchpad' is the left click, Back stays the right click and
     * the real touchpad click keeps its fingers. */
    ButtonMap pad=pad_map;
    assert(apply_button_map(&pad,"cross=b,circle=a,touchpad=misc1",false)==3);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_EAST)==BTN_CROSS);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CIRCLE);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==CLICK_LEFT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_TOUCHPAD)==BTN_TOUCHPAD);
    assert(apply_button_map(&pad,"touchpad_right=guide,touchpad_left=paddle1",false)==2);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_GUIDE)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)==CLICK_LEFT);
    assert(apply_button_map(&pad,"cross=Space",false)==0); /* a key name is no gamepad button */
    /* Keyboard: each side of the touchpad has a key of its own. */
    assert(pressed_by(&key_map,SDL_SCANCODE_TAB)==CLICK_LEFT && pressed_by(&key_map,SDL_SCANCODE_BACKSPACE)==CLICK_RIGHT);
    ButtonMap sides=key_map;
    assert(apply_button_map(&sides,"touchpad_right=Delete",true)==1);
    assert(pressed_by(&sides,SDL_SCANCODE_BACKSPACE)==0 && pressed_by(&sides,SDL_SCANCODE_DELETE)==CLICK_RIGHT);
    /* Comma and Equals stand for the two separator keys; Escape is an ordinary key. */
    ButtonMap marks=key_map;
    assert(apply_button_map(&marks,"cross=Comma,circle=equals,square=Escape",true)==3);
    assert(pressed_by(&marks,SDL_SCANCODE_COMMA)==BTN_CROSS && pressed_by(&marks,SDL_SCANCODE_EQUALS)==BTN_CIRCLE);
    assert(pressed_by(&marks,SDL_SCANCODE_ESCAPE)==BTN_SQUARE && pressed_by(&marks,MOUSE_LEFT)==0);

    /* Mouse names on the keyboard map (any case); the gamepad map has none. */
    ButtonMap mouse=key_map;
    assert(apply_button_map(&mouse,"cross=Mouse Middle,l1=Wheel Up,r1=wheel down,l2=Mouse Back,r2=Mouse Forward",true)==5);
    assert(pressed_by(&mouse,MOUSE_MIDDLE)==BTN_CROSS && pressed_by(&mouse,SDL_SCANCODE_Q)==0);
    assert(pressed_by(&mouse,WHEEL_UP)==BTN_L1 && pressed_by(&mouse,WHEEL_DOWN)==BTN_R1);
    assert(pressed_by(&mouse,MOUSE_BACK)==BTN_L2 && pressed_by(&mouse,MOUSE_FORWARD)==BTN_R2);
    assert(apply_button_map(&pad_map,"cross=Mouse Left",false)==0);

    /* Sources: mouse masks, wheel pulses, and keys only inside the scancode range. */
    static bool held[SDL_SCANCODE_COUNT];
    held[SDL_SCANCODE_E]=true;
    const bool no_wheel[2]={false,false}, up[2]={true,false};
    assert(source_down(SDL_SCANCODE_E,held,0,no_wheel) && !source_down(SDL_SCANCODE_Q,held,0,no_wheel));
    assert(source_down(MOUSE_LEFT,held,SDL_BUTTON_LMASK,no_wheel));
    assert(!source_down(MOUSE_RIGHT,held,SDL_BUTTON_LMASK,no_wheel));
    assert(source_down(MOUSE_BACK,held,SDL_BUTTON_X1MASK,no_wheel));
    assert(source_down(MOUSE_FORWARD,held,SDL_BUTTON_X2MASK,no_wheel));
    assert(source_down(WHEEL_UP,held,0,up) && !source_down(WHEEL_DOWN,held,0,up));

    /* Two notches up: press, gap, press, then idle; the other direction stays up. */
    SDL_SetAtomicInt(&wheel_ticks[0],2);
    const uint64_t t0=1000;
    assert(wheel_held(0,t0) && wheel_held(0,t0+WHEEL_PRESS_US-1));
    assert(!wheel_held(0,t0+WHEEL_PRESS_US));
    const uint64_t t1=t0+WHEEL_PRESS_US+WHEEL_GAP_US;
    assert(wheel_held(0,t1) && !wheel_held(0,t1+WHEEL_PRESS_US));
    assert(!wheel_held(0,t1+WHEEL_PRESS_US+WHEEL_GAP_US));
    assert(!wheel_held(1,t1));
    /* Notches beyond the limit are dropped, and the menu drops the rest. */
    SDL_Event spin={0};
    spin.type=SDL_EVENT_MOUSE_WHEEL; spin.wheel.integer_y=-10;
    mouse_watch(NULL,&spin);
    assert(SDL_GetAtomicInt(&wheel_ticks[1])==WHEEL_MAX_PENDING);
    drop_wheel();
    assert(SDL_GetAtomicInt(&wheel_ticks[1])==0 && !wheel_held(1,t1));

    /* The pause key tells the window once per press; L1 + touchpad right does not. */
    PadData menu={0};
    menu.buttons=CLICK_RIGHT; note_pause_key(&menu); note_pause_key(&menu);
    assert(pause_keys==1);
    menu.buttons=0; note_pause_key(&menu);
    menu.buttons=CLICK_RIGHT|BTN_L1; note_pause_key(&menu);
    assert(pause_keys==1);
    menu.buttons=0; note_pause_key(&menu);
    menu.buttons=CLICK_RIGHT; note_pause_key(&menu);
    assert(pause_keys==2);

    /* Mouse look: the right stick follows the mouse's movement, as far as it moves fast, and
     * is left alone again once the mouse has stopped. */
    SDL_Event glide={0};
    glide.type=SDL_EVENT_MOUSE_MOTION;
    PadData view={0};
    view.right_x=view.right_y=128; view.timestamp=50000000;
    apply_mouse_look(&view);
    assert(view.right_x==128 && view.right_y==128);              /* no motion: untouched */
    glide.motion.xrel=12.0f; mouse_watch(NULL,&glide);
    view.timestamp+=16000; apply_mouse_look(&view);
    assert(view.right_x>128 && view.right_x<255 && view.right_y==128);
    view.right_x=view.right_y=128; view.timestamp+=LOOK_HOLD_US+1; apply_mouse_look(&view);
    assert(view.right_x==128);                                   /* stopped: the pad's stick again */
    glide.motion.xrel=-2000.0f; glide.motion.yrel=-2000.0f; mouse_watch(NULL,&glide);
    view.timestamp+=16000; apply_mouse_look(&view);
    assert(view.right_x==0 && view.right_y==0);                  /* fast up and left: full tilt */
    look_sensitivity=50;
    glide.motion.xrel=12.0f; glide.motion.yrel=0.0f; mouse_watch(NULL,&glide);
    view.right_x=view.right_y=128; view.timestamp+=16000; apply_mouse_look(&view);
    const int slow=view.right_x-128;
    look_sensitivity=100;
    mouse_watch(NULL,&glide);
    view.right_x=view.right_y=128; view.timestamp+=16000; apply_mouse_look(&view);
    assert(slow>0 && view.right_x-128>slow);                     /* half the sensitivity: less tilt */

    /* Direction sources drive both sticks, release their old keys, and never reach PS4 buttons. */
    const ButtonMap original_keys=key_map;
    key_map=directions;
    static bool keyboard[SDL_SCANCODE_COUNT];
    keyboard[SDL_SCANCODE_W]=keyboard[SDL_SCANCODE_UP]=keyboard[SDL_SCANCODE_RIGHT]=true;
    PadData remapped={.left_x=128,.left_y=128,.right_x=128,.right_y=128};
    sample_keyboard(&remapped,keyboard,0,no_wheel);
    assert(remapped.left_y==128 && remapped.right_x==128 && remapped.right_y==128);
    keyboard[SDL_SCANCODE_T]=keyboard[SDL_SCANCODE_H]=keyboard[SDL_SCANCODE_KP_4]=true;
    sample_keyboard(&remapped,keyboard,0,no_wheel);
    assert(remapped.left_x==255 && remapped.left_y==0 && remapped.right_x==0);
    assert(!(remapped.buttons & STICK_DIRECTIONS));
    assert(apply_button_map(&key_map,"move_up=None,move_right=None,look_left=None",true)==3);
    remapped=(PadData){.left_x=128,.left_y=128,.right_x=128,.right_y=128};
    sample_keyboard(&remapped,keyboard,0,no_wheel);
    assert(remapped.left_x==128 && remapped.left_y==128 && remapped.right_x==128);
    ButtonMap untouched_pad=pad_map;
    assert(apply_button_map(&untouched_pad,"move_up=a,look_right=b",false)==0);
    assert(!memcmp(&untouched_pad,&pad_map,sizeof(pad_map)));
    key_map=original_keys;

    puts("PASS: pad and key maps");
}
