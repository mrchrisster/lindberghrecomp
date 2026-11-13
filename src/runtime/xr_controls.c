#include "xr_controls.h"
#include "cabinet_buttons.h"
int lg_xr_controls_select(LgXrControls *s,const int trigger[2],int dual)
{
    for(int h=0;h<2;++h)if(!dual&&trigger[h]&&!s->prev_trigger[h]&&h!=s->gun_hand)s->gun_hand=h;
    return s->gun_hand;
}
/* A thumbstick as the 8-way lever: past a dead zone, an axis counts once it
 * is over tan(22.5 deg) of the other, so each of the 8 sectors is 45 deg. */
static unsigned lever(float x,float y)
{
    unsigned b=0;const float ax=x<0?-x:x,ay=y<0?-y:y;
    if(x*x+y*y<.35f*.35f)return 0;
    if(ax>ay*.414f)b|=x<0?CAB_P1_LEFT:CAB_P1_RIGHT;
    if(ay>ax*.414f)b|=y>0?CAB_P1_UP:CAB_P1_DOWN;
    return b;
}
/* Virtua Tennis 3 in the headset: either thumbstick is the lever. The
 * cabinet has two shot buttons - its How to Play: left button top spin,
 * right button backspin (slice), both together a lob - so a lob is both
 * switches at once (button 1 = switch 0x02, button 2 = 0x01; the game's pad
 * reads no other shot switch: "button 3" was never a shot, and B did
 * nothing). Right hand: trigger top spin, A slice, B lob; left hand the
 * same on its trigger, X and Y. */
static unsigned lever_buttons(const LgXrControlFrame *f)
{
    if(f->blocked)return 0;
    unsigned b=lever(f->thumb_x[0],f->thumb_y[0])|lever(f->thumb_x[1],f->thumb_y[1]);
    for(int h=0;h<2;++h){
        if(f->trigger[h])b|=CAB_P1_TRIGGER;
        if(f->lower[h])b|=CAB_P1_BUTTON2;
        if(f->upper[h])b|=CAB_P1_TRIGGER|CAB_P1_BUTTON2;
    }
    return b;
}
LgXrCabinet lg_xr_controls_update(LgXrControls *s,const LgXrControlFrame *f)
{
    LgXrCabinet out={0,0};const int g=s->gun_hand,off=1-g;
    if(f->joystick)out.held|=lever_buttons(f);
    else for(int pl=0;pl<(f->dual?2:1);++pl) {
        const int h=pl?off:g;unsigned b=0;
        if(f->fire_supplied?f->firing[pl]:(f->trigger[h]&&f->aimed[pl]&&!f->blocked))b|=CAB_P1_TRIGGER;
        if(f->trigger[h]&&!f->aimed[pl]&&!f->blocked)b|=CAB_P1_RELOAD;
        if(f->grip[h]&&!s->prev_grip[h])s->grip_at[h]=f->time;
        if(f->grip[h]&&!f->blocked&&f->time-s->grip_at[h]<120000000)b|=CAB_P1_RELOAD;
        if(f->lower[h]&&!f->blocked)b|=CAB_P1_ACTION;
        if(f->upper[h]&&!f->blocked)b|=CAB_P1_FIREMODE;
        out.held|=pl?b<<CAB_P2_SHIFT:b;
    }
    int start=(f->dual?f->stick[g]:f->stick[0]||f->stick[1])&&!f->blocked;
    int start2=f->dual&&f->stick[off]&&!f->blocked;
    if(f->dual&&f->trigger[off]&&!s->prev_trigger[off]&&!f->blocked&&
       (!s->joined_at||f->time-s->joined_at>3000000000LL)) {
        s->joined_at=f->time;out.coins+=f->auto_coins;
    }
    const int join_start=f->dual&&s->joined_at&&f->time-s->joined_at>=150000000&&f->time-s->joined_at<450000000;
    if(f->start_override[0])start=1;
    if(f->start_override[1]&&f->dual)start2=1;
    if(start)out.held|=CAB_P1_START;
    if(start2)out.held|=CAB_P2_START;
    if(f->auto_coins) {
        /* A lever cabinet (Virtua Tennis 3): a coin the game takes ~2 s to count
         * makes a Start pressed with it go unheard (its continue screen: each
         * stick click added a coin and nothing else - the owner's "only add
         * coins", 2026-11-13). So the coin comes when the game asks for one
         * (coin_wanted: the continue screen opening), and a Start press adds one
         * only when no automatic coin came in the last 10 s - a second press is
         * a plain Start. Gun games: a coin with every Start, as before. */
        const int lever_cab=f->joystick;
        const int cool=lever_cab&&s->auto_coin_at&&f->time-s->auto_coin_at<10000000000LL;
        if(++s->credit_frames==240){++out.coins;s->auto_coin_at=f->time;}
        if(lever_cab&&f->coin_wanted){++out.coins;s->auto_coin_at=f->time;}
        if(start&&!s->prev_start[0]&&!cool){++out.coins;s->auto_coin_at=f->time;}
        if(start2&&!s->prev_start[1]&&!cool){++out.coins;s->auto_coin_at=f->time;}
    }
    if(join_start&&!f->blocked)out.held|=CAB_P2_START;
    s->prev_start[0]=start;s->prev_start[1]=start2;
    for(int h=0;h<2;++h){s->prev_trigger[h]=f->trigger[h];s->prev_grip[h]=f->grip[h];}
    return out;
}
