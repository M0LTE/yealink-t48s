# The phone: dialler, calls, history, contacts

Issues [#26](https://github.com/M0LTE/yealink-t48s/issues/26), [#27](https://github.com/M0LTE/yealink-t48s/issues/27) (contacts, do not disturb, a second call, the message lamp) and [#36](https://github.com/M0LTE/yealink-t48s/issues/36) (several accounts). Design: [architecture.md](architecture.md#the-shell). Source: [`shell/phone.c`](../shell/phone.c), [`contacts.c`](../shell/contacts.c), with [`sipc.c`](../shell/sipc.c) (baresip), [`audioc.c`](../shell/audioc.c) (the audio service) and [`leds.c`](../shell/leds.c) (lamps).

## Summary

- The shell now behaves like a desk phone. Lift the handset for dial tone, or just start typing a number. Calls ring, can be answered with the handset, Speakerphone or Headset, and can be muted, held, transferred and ended the usual ways.
- A call takes the screen from whatever is showing, including an app, and gives it back afterwards.
- **New (2026-11-23, #27):** a contact list kept on the phone, with names shown wherever a number is; do not disturb; a second call (call waiting, hold one and talk to the other, swap); asking someone first before a transfer; the lamp at the top right as the ringing, voicemail and missed-call lamp; the Message key calls voicemail.
- **New (2026-11-23, #36):** up to six SIP accounts, each with its own registration, and a choice of which one a call goes out from. See [Several accounts](#several-accounts).
- **Tested in the simulator against the real services**: baresip with our modules, `t48-audiod` on its silent backend and a throwaway SIP server, with two more baresips as the people at the other end. Eighteen scripted scenarios pass, and pass again with the phone's own ARM builds of the shell, baresip and the audio service running under qemu. The new parts (#27, #36) have not run on the phone yet.
- Apps can no longer reach call control: baresip's TCP control port is gone, replaced by our own module on a socket only root can open ([sip.md](sip.md#control-interface)).

| | |
|---|---|
| ![idle](img/phone-idle.png) | ![dialler](img/phone-dialler.png) |
| ![outgoing](img/phone-outgoing.png) | ![incoming](img/phone-incoming.png) |
| ![in call](img/phone-in-call.png) | ![muted, on the headset](img/phone-in-call-muted.png) |
| ![on hold](img/phone-in-call-held.png) | ![tone keypad](img/phone-in-call-keypad.png) |
| ![transfer](img/phone-transfer.png) | ![call ended](img/phone-ended.png) |
| ![a call that failed](img/phone-call-failed.png) | ![history](img/phone-history.png) |
| ![missed call on the idle screen](img/phone-idle-missed.png) | ![volume](img/phone-in-call-volume.png) |
| ![on the speaker](img/phone-in-call-speaker.png) | ![a call arriving over an app](img/phone-incoming-over-app.png) |
| ![a second call waiting](img/phone-call-waiting.png) | ![two calls, one on hold](img/phone-two-calls.png) |
| ![asking first before a transfer](img/phone-ask-first.png) | ![talking to them, the caller on hold](img/phone-ask-first-talking.png) |
| ![contacts](img/contacts.png) | ![a caller who is in the contact list](img/phone-incoming-contact.png) |
| ![messages waiting](img/phone-messages.png) | ![do not disturb](img/phone-dnd.png) |
| ![two accounts on the idle screen](img/phone-idle-two-accounts.png) | ![choosing the account to call from](img/phone-dial-account.png) |
| ![which account was called](img/phone-incoming-account.png) | ![the history with accounts](img/phone-history-accounts.png) |

## What each thing does

| | Idle | Dialling | Ringing (incoming) | Calling or in a call |
|---|---|---|---|---|
| **Lift the handset** | dial tone | calls the number typed; else dial tone | answers | moves the sound to the handset |
| **Put it down** | | closes the dialler | | ends the call, if the sound was on the handset |
| **Speaker**, **Headset** | dial tone there | calls the number typed there | answers there | moves the sound there. Pressed again: back to the handset if it is lifted, otherwise ends the call |
| **Digits, `*`, `#`** | start a number | add to it | | send tones |
| **OK** | apps | calls (on the speaker if the handset is down) | answers | answers a call that is waiting; with a call on hold, swaps to it |
| **X** | | rubs out a digit; closes when empty | declines | cancels a call that has not connected; turns away a call that is waiting; closes the keypad |
| **Mute** | | | silences the ringer | microphone off and on; silences the call waiting beep |
| **Hold** | | | | hold and resume |
| **Transfer** | | | | asks for a number; then Transfer sends the caller there at once, OK calls them first. With one call on hold and one in front, puts the two through to each other |
| **Redial** | calls the last number dialled, from the account it was dialled from | the same | | |
| **Message** | calls the voicemail number | the same | | |
| **Volume** | ringer | where the sound is | ringer | where the sound is |
| **Up** | call history | | | |
| **Left** | contacts | chooses the account to call from | | |
| **Right** | do not disturb on and off | chooses the account to call from | | |

Other things worth knowing:

- Volumes are kept per place (handset, speaker, headset, ringer) in `/data/config/audio` and given to the audio service at start.
- The touch screen has the same things as buttons: a keypad in the dialler and in a call, Answer and Decline, Mute, Hold, Keypad, Speaker, Transfer, End. The on-screen Speaker button only moves the sound; it never hangs up.
- Two calls at most; a third caller hears busy.
- A call that fails plays busy or error tone in the handset (or on the speaker) until you hang up. With the phone on the hook nothing is played.
- Missed calls are counted on the idle screen until the history has been looked at. History is the last 50 calls, in `/data/phone/history`. Picking one puts its number in the dialler. Its buttons: Dial, Add contact (for a number not in the list yet), Delete (that line) and Clear all (asks first).
- If the shell is restarted in the middle of a call, the call carries on (it lives in baresip and the audio service) and the new shell picks it up: same screen, same timer, same route and mute.
- If baresip stops, the idle screen says "No phone service" and a call in progress ends as "Call lost".

Not done: a three-way conference; call forwarding; speed-dial line keys; dialling a number by itself after a pause; a choice of ringing tones (the audio service has one melody; the ringer's volume is in Settings, Sound and on the volume keys).

## A second call

- **Call waiting.** A call that arrives while you are on another is shown in a line across the call screen, the lamp blinks and the audio service plays a quiet beep where the sound is. OK or Answer takes it and puts the first call on hold. X or Decline tells the caller busy. Mute stops the beep. If the caller gives up it is a missed call.
- **Two calls.** The line then says who is on hold. OK or Swap changes over. End ends the one you are talking to and the other comes straight back. Putting the handset down ends the one you are talking to and leaves the other on hold, saying so on the screen; lifting the handset or pressing Speaker carries on with it.
- **Asking first (attended transfer).** Transfer, type the number, then OK or "Ask first": the caller is put on hold and the number is called. X gives up and goes back to the caller. Once they answer, Transfer or Join puts the two through to each other and takes this phone out of it. Transfer pressed instead of OK is the blind transfer as before.
- Join also works on two calls that started as call waiting.

## Contacts

Left on the idle screen, or the Contacts button. A list of names and numbers, in name order, up to 200.

- Add, Edit and Delete (which asks first) are buttons under the list; names and numbers are typed with the same keyboard as Settings ([settings.md](settings.md#the-keyboard)).
- OK, Dial, or a second touch on a contact puts the number in the dialler.
- A number in the list shows its name everywhere: the dialler as you type, calls going out, calls coming in (in place of the name the caller sends), a waiting or held call, and the history. The history's Add contact button starts a new contact with the number filled in.
- The file is `/data/phone/contacts`, one contact a line, `name|number`. A list made elsewhere can be imported as CSV or vCard on the web setup page ([web.md](web.md)), or copied there over SSH; the shell reads the file at start and again whenever it changes.

| | |
|---|---|
| ![typing a number](img/contacts-number.png) | ![deleting asks first](img/contacts-delete.png) |
| ![from the list to the dialler](img/contacts-dial.png) | ![the history with names](img/phone-history-names.png) |

## Do not disturb

Right on the idle screen, or the DND button. While it is on, callers are told the phone is not available (SIP 480), nothing rings, and each call is listed as missed. Calls out work as usual. It is kept in `/data/config/phone` (`dnd=on`) so it survives a restart, and the idle screen says so in red.

## Messages waiting

For each account the phone asks the server to tell it about waiting voicemail (SIP message-summary, RFC 3842). While there are new messages the lamp blinks slowly and the idle screen says how many. The Message key calls the account's voicemail number, set in Settings, Phone accounts ("Voicemail number"). A server without voicemail just says no and nothing more happens.

## Several accounts

Up to six, set in Settings, Phone accounts ([settings.md](settings.md)). Account 1 is `/data/config/sip`, as it always was, so a phone that already has an account needs nothing done; accounts 2 to 6 are `sip2` to `sip6` beside it.

- **All of them ring.** The incoming call screen says which account was called.
- **Calls go out from the usual account** unless another is chosen: Left and Right in the dialler step through the accounts, as does a touch on the "From" line. A touch on an account's line on the idle screen opens the dialler already set to it.
- **Which is usual** is set on the Phone accounts page. The status bar shows that one; with more than one account the idle screen has a line for each with its own dot.
- **The history keeps the account** of each call, and Redial and the history call back from the same one.
- An account can be switched off without forgetting it ("In use"), given a short name for the phone's own screens, and removed.

The dot beside an account is green when registered, amber while trying, red when the server refused or baresip is not running. The shell reads the same files `t48-sip` does ([sip.md](sip.md#running-it), example in [`sip/config/sip.example`](../sip/config/sip.example)), and only for what it shows; the passwords are baresip's business. No account is in the repository; the tests use made-up ones on a server that exists for a minute.

The T48S has no line keys beside the screen and no lamps for them (the screen itself is where Yealink draws them), so the account lines on the idle screen are the line keys.

## Lamps

The phone decides what each lamp should do, and [`shell/leds.c`](../shell/leds.c) passes that to the kernel's LED driver ([#3](https://github.com/M0LTE/yealink-t48s/issues/3), [kernel/README.md](../kernel/README.md)) through `/sys/class/leds`:

| Lamp | When |
|---|---|
| message (top right of the case) | blinks fast while a call is ringing or waiting (0.3 s on, 0.3 s off, as Yealink does); blinks slowly while voicemail waits (1 s on, 1 s off); on while there are missed calls |
| speaker, headset (in those keys) | on while the sound is there |
| mute (in the Mute key) | on while muted |

The message lamp was called "power" here until 2026-11-23 (Yealink's own files call it that). The kernel driver now names it `t48s:red:message`; the shell also accepts the old name, so it works with a kernel built before the change.

## How it fits together

```
 keys, hook, touch            baresip (t48ctrl)                  t48-audiod
        |                 /run/t48-sip/ctrl.sock           /run/t48-audiod/ctl.sock
        v                          ^                                 ^
   main.c  --->  phone.c  ---  sipc.c: dial, accept, hangup,   audioc.c: route, volume,
   (screens)   (one call,      hold, resume, transfer,         mute, tone, ring, key
                the rules)     sndcode; events back            sounds; state back
```

`phone.c` is one small state machine: idle, dial, outgoing, incoming, in call, ended, for the call in front; beside it sits at most one other call, either waiting to be answered or on hold. It follows baresip's events (`CALL_INCOMING`, `CALL_RINGING`, `CALL_ESTABLISHED`, `CALL_CLOSED`, `REGISTER_OK`, ...) and tells the audio service what the owner should hear, in the order [audiod.md](audiod.md#the-control-socket-the-shell) asks for. Both clients keep trying once a second if their service is not there, and say everything again when it comes back.

## Testing

```
make -C shell check          # no services needed: includes the phone screens on their own
sip/test/phone-test.sh       # the real thing; in the sip/Dockerfile container (see sip.md)
ARM=1 sip/test/phone-test.sh # the same, with the phone's side as the ARM builds under qemu-arm
```

`phone-test.sh` starts Kamailio, the phone's `t48-audiod` (null backend) and baresip registered as 1001, two far ends, 1002 and 1003, and `mwi.py`, which plays a voicemail server's message-waiting side. Then it runs each script in [`shell/tests/phone/`](../shell/tests/phone) through `t48-sim`. The scripts press keys, lift the handset and touch the screen; make the far ends dial, answer and hang up; check the phone's state, the audio service's own state and what the far ends saw; and save pictures. The pictures on this page are from that run.

Result on 2026-11-23, all passing, both as PC builds and with `ARM=1`:

| Scenario | What it shows |
|---|---|
| 01 outgoing, answered | type 1002, lift the handset: the call goes out on the handset, ringback plays, the far end answers, tone stops, the audio service has the call's sound; handset down ends it and the far end sees it close |
| 02 incoming, answered by hook | the ringer and the lamp start; lifting the handset answers and stops them; the far end hangs up and the phone goes back to idle by itself |
| 03 speakerphone | answered with Speaker; handset lifted takes the sound; Speaker gives it back; the handset can go down without ending the call; Speaker again ends it |
| 04 mute, hold | on the headset: Mute reaches the audio service and back; Hold and resume reach the far end |
| 05 far end hangs up | dialled with the handset down (OK): goes out on the speaker; far end hangs up; Redial calls the same number; X cancels |
| 06 call while an app is open | the clock app is paused and frozen, the call screen takes over, keys go to the call, and the app is back and running when the call ends |
| 07 keypad and volume | dial tone on lifting the handset, stopping at the first digit; tones sent from the touch keypad arrive at the far end; Volume changes the handset's level in the audio service, not the ringer's |
| 08 blind transfer | the caller is sent to 1003, 1003 rings and answers, our call ends as transferred |
| 09 missed call, history | Mute silences the ringer; an unanswered call is counted on the idle screen; a call that is turned down plays busy tone until the handset goes down; the history lists both and clears the count |
| 10 call waiting, declined | a second caller shows on the call screen with the beep and the lamp; Mute stops the beep; X tells them busy; one who gives up is a missed call; the first call is not disturbed |
| 11 shell restart | the shell exits in the middle of a muted speakerphone call; a new one finds the call, the route and the mute, and ends it normally |
| 12 call waiting, answered | OK answers the second call and the first far end sees itself held; OK and the Swap button change over, each far end seeing hold and resume; End brings the held call back; both are in the history |
| 13 attended transfer | Transfer, a number, OK: the caller is held and 1003 rings; X goes back to the caller; again, 1003 answers, Transfer puts 1002 and 1003 through to each other and our calls end as transferred |
| 14 do not disturb | the caller gets 480, nothing rings, the call is listed as missed; calls out still work; off again and the next call rings |
| 15 message waiting | the server says two messages: the idle screen and the slow lamp; the Message key calls voicemail; a ringing call takes the lamp and gives it back; the server says none and the lamp shows the missed call alone |
| 16 contact names | a contact added on the phone names the caller in place of what the caller sends, in the call, the history and when dialled from the list; a stranger keeps their own name |
| 20 several accounts | a second account is typed in on the Settings screens and the phone service restarts with both registered; a call out from the chosen account arrives from 1004, Redial uses it again, a plain call still goes from 1001; calls in to each say which was called; the history has the account; the usual account is changed and changed back; the account is removed and one is left |

`make -C shell check` also runs `08-contacts.script`: the contact list, the history's buttons and do not disturb with no services behind them.

Also checked by hand in the container, with baresip running as root: root can connect to the control socket; another user gets "permission denied" (the folder and the socket are closed to it); and with those opened up on purpose the module still turns that user away by its user number. The older tests (`call-test.sh`, `audiod-test.sh`) now drive baresip through the same socket and still pass, including the ARM baresip under qemu.

## What needs the phone, and Tom

1. **Sound.** Everything here asks the audio service for the right thing; whether the right thing comes out waits on the kernel's playback driver and the daytime audio tests ([audiod.md](audiod.md#what-needs-the-phone-and-tom)).
2. **A real account.** Put one in `/data/config/sip`, start `t48-audiod`, `t48-sip` and the shell, and make a call each way with fingers. The key meanings above are what a desk phone does, but only using it will say if any feel wrong.
3. **Six function keys still need their labels matched** (kernel/README.md): if Hold, Transfer or Redial turn out to be different keys, the fix is in the kernel's key table, not here.
4. **Lamps:** the driver is in; someone has to look at them once (the list is in issue #3).
5. **Being transferred by the other end**: `t48audio` now carries more than one call, which this needed, but it has still not been tried.
6. **The new parts** (#27, #36), each once with fingers: a second caller while on a call (the beep in the earpiece, answer, swap, end one); Transfer with "Ask first"; DND; a contact; and, if the server has voicemail, the lamp. Then a second account.
7. **The lamp's new name** needs a kernel built from this branch; until then the shell finds it under the old name. Someone should still look at it blinking fast (ringing) and slowly (voicemail).
