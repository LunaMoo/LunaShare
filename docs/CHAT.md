# Chats

## Links

Links in messages - `https://...`, `http://...` and `www....` - are shown as links; a tap (or click) opens them in
the system's browser. Hovering one on a computer shows where it goes.

## Disappearing messages

*Chat history > Messages disappear*: never, after 1 hour, 1 day or 1 week. It's the same for everyone in the chat
(whoever changes it last wins; the chat notes each change, and the header shows a *disappear* badge). New text
messages carry how long they have left (`ttl`, seconds) and each device removes them when that's up (checked every
few seconds, also from the saved history). Files that came are left where they were saved. It doesn't stop
anyone copying or photographing a message before it goes. (Message type 107 `ChatTtl` `{ttl, ts}`.)

## Pinned and starred

*Pin in this chat* (a message's menu) pins it for everyone in the chat: a bar under the chat's name shows it, a
tap goes there (with several: the next tap to the next one), x unpins. The latest pin / unpin wins; whoever missed it
gets it when they connect (message type 105 `ChatPin`). *Star* is only yours: *Chat history > Starred messages*
lists the starred messages of all chats, a tap opens the chat at that message.

## Forwarding

*Forward...* in a message's menu (or *Forward* in the toolbar with several selected) opens the chat list: the text
goes as new messages marked *forwarded*, the files (when they're on this device) as a new transfer. Groups get
text only.

## Drafts

Text you typed and didn't send stays in its chat when you switch to another one or close LunaShare (kept in
`drafts.json`, on this device only). The chat list shows *Draft: ...* under the name.

## Silent chats

*Silence notifications...* (a chat's menu in the list - long press / right click - or the *Chat history* window):
for 1 hour, 8 hours, a day, a week or until you turn them back on. New messages in a silent chat show no
notification and no message popup; they're counted in grey (and not in the window title). Calls still ring. The
list shows *silent* under the name; *Turn notifications back on* in the same places ends it early.

## Replying and changing messages

A message's menu (long press on a phone, right click on a computer) has *Reply*: a strip above the typing field
shows which message you answer (x stops). The answer carries the message it answers - who wrote it and its start -
shown on top of it; a tap there scrolls to that message. *Change...* (your own text messages) puts the text back in
the typing field; sending replaces it everywhere, marked *changed* next to the time. Changes reach devices that are
away when they come back (or through the server's mailbox), and the newest change wins.

## Searching

The magnifier in a chat's header (on a phone: *Search messages* in the chat's history window) opens a search bar:
type, then *Enter* or the arrows go through the matches from the newest back, jumping to each message and lighting
it up for a moment. *in all chats* lists what's found in every chat (newest first); tap one to open it there.
Letters are matched without minding upper / lower case (Polish letters too).

## Chat history: archive, clear, restore

The clock button in a chat's header opens *Chat history*:

- **Archive** saves the whole conversation to a compressed archive on this device (`archives/` in the settings
  folder). Files aren't copied: messages keep their links to them, so a file that's still on the disk opens from a
  restored chat as before. *Then clear the chat* removes the archived messages afterwards - with *Keep yesterday's
  and today's messages* if you like (messages still waiting to be sent always stay).
- **Archives** lists this chat's archives (or all chats', with *Show the archives of all chats*): when, how many
  messages, from when to when, the size. *Restore* puts back what the chat doesn't have (nothing is doubled);
  *Delete...* removes an archive for good.
- **Lost the history?** When both the chat and its archive are gone, *Ask ... to send it back* asks the other side -
  and your own other devices, which have the same chats. They see the request in the chat and choose *Send it* or
  *Don't send*; what comes back joins the chat with its sides right (your messages stay yours). Files that came along
  with it link to the copies in your received files folder when those are still there.

Archives are kept per device - they aren't synced.
