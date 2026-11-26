# Words and look

One voice for the phone's screen ([`shell/`](../shell), [`libt48/`](../libt48)) and the web setup page ([`webconfig/`](../webconfig)). The look is in [design/](design/README.md); this page is the wording, and it applies to every string a person can read: labels, buttons, notices, errors, the app manager's messages, and apps that want to fit in.

## Voice

1. **Say it once.** If the label, the control or the state already says it, the sentence goes.
2. **State, not story.** `Registered`, `Off`, `No network`. Not what happened, why, or what it means.
3. **The reader is competent.** No definitions of SIP, DHCP, SSH or a public key. No reassurance, no advice, no "you".
4. **Hints are rare.** Only where an action is not obvious from the screen, and then a few words: `Lift handset to resume`.
5. **No decoration.** No exclamation marks, no emoji, no "please", "simply", "just", "oops", "let's". No em or en dashes; ASCII only on the phone.

| Before | After |
|---|---|
| Include the passwords. The file is then marked as sensitive: keep it somewhere safe | Include passwords |
| OK apps, Up calls, Left contacts, Right do not disturb, Down settings, or just dial | (removed) |
| Lift the handset, or press Speaker or Headset, to answer | (removed: the Answer button is there) |
| Up and Down to choose, OK or a touch to put the number in the dialler, X to go back | (removed) |
| The screen dims when nothing has been touched for a while, and later switches off. Any key or a touch brings it back. | Dim after: 2 min. Off after: 30 min |
| Anyone on your network holding a key this phone accepts gets full control of it. | Root access for anyone on the network with an accepted key. |
| If that is you, type this code into the page: | Pairing code |
| Not you? Press X, or touch here, and the code is thrown away. | X to refuse |
| Account 1 is saved. The phone service is starting again with it. | Account 1 saved. |
| That code is no longer good: it ran out, was refused at the phone, or was asked for by another browser. Ask for a new one. | Code expired or refused. Request a new one. |

## One word for one thing

| Thing | Word | Not |
|---|---|---|
| A SIP identity | Account | Phone account, SIP account, line |
| The account calls go out from | Default | Usual |
| Account switched on or off | Enabled | In use |
| Account states | Registered, Registering, Not registered, Off | registered OK, failed |
| baresip not reachable | No service | No phone service, the SIP service is not running |
| The call list | Calls | History, call history, log |
| Where the sound goes | Handset, Speaker, Headset, Ringer | Speakerphone, loudspeaker |
| A call parked by us, by them | On hold, Held remotely | Held by the other end |
| Take out something installed or added (app, account, key) | Remove | Forget, uninstall |
| Take out an entry (contact, call) | Delete | Remove, forget |
| Empty a list | Clear | Forget all |
| Back out of a question | Cancel | Keep, Keep them, Keep off |
| A switch | On, Off; the action is Turn on, Turn off | Enable, activate |
| The browser page | Web setup | Web setup page, the web page |
| Admit a browser with a code | Pair | Link, connect |
| End a browser's pairing | Sign out | Forget, unpair |
| The six digits | Code | PIN, number |
| The list apps come from | Feed, Feeds, Add feed; the built-in one is Official | Feed address, the standard feed, Default feed, repository |
| Fetch the list again | Refresh | Look again |
| An app's factory values | Reset to defaults | Back to the app's own defaults |
| An app that ships in the image | Built in | Part of the firmware |
| SSH public keys | Keys, Add key, Host key | Keys that can log in |
| Address source | DHCP, Static | From the network, a fixed address |
| Account fields | User, Password, Server host, Outbound proxy, Auth user, Display name, Transport, Account name, Voicemail; under Advanced: Registration expiry, DTMF, Codecs, NAT keepalive, Session timer, 100rel, SRTP | Domain, Proxy, SIP server, Server or proxy (optional), Login name, if not the user, Label |
| DTMF modes | RFC 2833, In-band, SIP INFO | RFC2833, Inband, SIP-Info |
| SRTP settings | Off, Optional, Compulsory, DTLS-SRTP | Disabled, Mandatory, Opportunistic |
| A call whose audio is SRTP | Encrypted | Secure, SRTP, a padlock |
| Root certificates added for TLS | Certificates; Add certificate | CA, trusted roots, custom CAs |
| The settings file download | Backup; Download, Restore | Save the settings to a file, Put a saved file back |

## Capitals and punctuation

- Sentence case everywhere: `Date and time`, `Add key`. Proper names and initialisms keep theirs: `SSH`, `DHCP`, `TLS`, `NTP`.
- Values start with a capital: `Not set`, `Registered`, `On`. Things the owner typed or the system named are shown as they are: `1001@pbx.example.org`, `Europe/London`.
- No full stop on a label, button, value, hint or one-sentence notice on the phone. Web notices that are sentences end with one.
- A colon joins a name to its state: `Home: not registered`. A comma joins two facts: `1.0.0, 12 KB`.
- No trailing ellipsis for progress: `Loading`, `Downloading`.

## Parts

**Labels** are nouns, one to three words: `Voicemail`, `Feed URL`. No "(optional)": an empty field is allowed unless the error says otherwise. On the web, "Optional" may be the placeholder.

**Buttons** are one verb, or verb and object when two buttons could be confused: `Call`, `Save`, `Remove`, `Add key`. A button that changes state keeps its name and lights up; `Mute` does not become `Unmute`.

**Status** is an adjective or a count: `Registered`, `3 installed`, `2 missed calls`.

**Notices** (something happened) name the thing and the result, three or four words: `Demo installed`, `Key added`, `Account 2 saved.` No second sentence unless it tells the owner something they must act on.

**Errors** say what is wrong, then the fix if it is not obvious. `Not an address` / `Use the form 192.168.1.20`. `Wrong code. 4 tries left.` Never "something went wrong", never an apology. A cause from the system is passed on as it is: `Demo not installed` / `Checksum mismatch`.

**Confirmations** ask with the verb and the object, and the buttons are `Cancel` and that verb: `Remove Clock?` `Cancel` `Remove`. One more line only for a consequence that is not obvious: `Deletes its data and settings`. On the web, a destructive form has a `Confirm` tick box and a button that names the action.

**Empty states** are `No` and the thing: `No calls`, `No contacts`, `No apps`.

**Hints** on the phone are the exception. The ones that remain: `Lift handset to resume` (a call parked with the handset down), `# abc` on the keyboard (the key that changes the keypad's letters), `X to refuse` on a pairing code, `Applied when you leave this page` on Network.

## Numbers and units

- Digits always: `6 accounts`, `10 minutes`.
- Time of day is 24 hour, `16:25`. Durations are `m:ss` or `h:mm:ss`; countdowns the same. In a sentence: `2 min`, `30 s`, `12 hours`.
- A space before a unit: `12 KB`, `1.4 MB`, `31 MB free`.
- A level is the bare number, `9`; where the range matters, `9 of 15`.
- Ranges use "to": `0 to 15`.
- Plurals are real: `1 call`, `2 calls`. Never `call(s)`.
- Dates: `Monday 23 November 2026` on the idle screen, `23 Nov 16:25` in lists.
