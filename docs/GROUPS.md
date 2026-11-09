# Groups: chats and calls with several people

## Group chats

*New group* (sidebar on computers, under the list on phones): a name and a tick for each person. Everyone in a group
can add people (*People in this group → Add people*) and rename it; everyone can leave; whoever created it can also
remove people. Only people with an account (LunaShare 1.1 or newer) can be in a group.

How it works:

- A group is a list of accounts and a version number. Whoever changes it sends the new list (`GroupInfo`) to every
  device of every member, together with the **signed device lists** of the members' accounts that they know. That way
  members who have never met can reach each other, and each device knows the others are who they say (the account
  key signed them). A newer version replaces an older one; a member can only take *themselves* off the list (the
  creator anyone), so nobody can silently throw others out.
- A message is sent to every device of every member separately, end-to-end encrypted like any other message, tagged
  with the group id. When someone is offline it waits in the server's mailbox, as usual. The server never sees
  anything about groups.
- Devices known only through a group are not listed as contacts. Writing to such a person directly makes them one.

## @mentions

Type `@` and the start of someone's name: the people in the group who match show above the typing field; a tap
puts their name in. The message carries who was named (`men`: their account keys). The one named gets a
notification even if the group is silent, the unread count shows `@`, and the message has an outline.

## Group calls

The phone and camera buttons in a group call everybody in it. Whoever answers joins; the call goes on as long as
someone is in it. Everyone hears everyone: the voice module (API 6) decodes each person separately and mixes them,
and the echo canceller hears the mix.

Video: you watch **one person at a time** - automatically the one with the camera on who talks, or whoever you tap
in the row of people at the top of the picture. Only the people watching you get your video, which keeps it light
for phones. Group video is limited to 720p / 30 fps.

### Directly or through the server

*Settings → Internet → Group chats and calls through the server* decides how a call you start travels:

- **Directly** (off): every device sends to every other one over the normal encrypted connections. With four people
  your phone uploads your voice three times (and your video to everyone watching you).
- **Through the server** (on): every device sends each packet **once** to the rendezvous server, which passes it to
  the people it is for (a "hub"). The packets are sealed with a key that only the people invited to the call get
  (in the invitation, which travels end-to-end encrypted), so the server can't hear or see anything - it only knows
  which devices are in the call. Pictures are split into pieces of about 1 KB with one parity piece per eight, so a
  lost piece is repaired without asking again. Group members you don't know otherwise never connect to your device
  directly, so they don't learn your internet address; messages to them go through the mailbox.

The server needs relaying allowed (`--no-relay` turns it off); `--relay-limit KBPS` limits what each person may send.

### Under the hood

- Invitation: `CallInvite {call, media, gid, ts[, hub, key]}` to every member device (or by mailbox; one older than a
  minute is ignored). Directly: whoever joins sends `CallJoin` to everyone, who answer the same way; through the
  server: who is in the call is the hub's member list.
- `CallWant {want}` tells someone "send me your video" / "stop" (asked again every few seconds).
- Video packets are routed by kind: frames only to the people watching, camera state and codecs to everyone,
  keyframe requests / size limits / receiver reports only to the person being watched. The video module hears the
  codecs everyone can decode, combined, so it picks one they all can.
- Hub datagrams: `'H' [hub u32][to mask u32][fragment]` up, `'H' [hub u32][from u8][fragment]` down; fragments
  `[msg u16][index u8][count u8][total u32][data]`, indices from `count` on are XOR parity over groups of eight.
  Messages inside are `KeySeal`ed (XChaCha20-Poly1305): `'M'` + media packet, or `'C'` + message type + JSON.
- At most 32 people per group and per call.
