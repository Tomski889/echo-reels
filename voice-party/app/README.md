# Echo Party prototype

Invite-only voice parties for Echo players. This build is a browser prototype:
the standalone web application lives here. The parent voice-party directory
contains the experimental native tablet host; see ../README.md.

## Try locally

Install Node.js 22 or newer. In this folder run `npm start`, then open
http://127.0.0.1:9630. Create your profile, share your friend ID, accept a friend
request, create a party and invite your friend. Each member clicks Connect
microphone. Mute, deafen and leave controls are available in the Party page.
The local server is accessible only on your own PC; friends on other PCs need
the shared HTTPS deployment below. Do not open a router port for this demo.

Your browser keeps your signing key in IndexedDB. Clearing browser data loses
your identity; account recovery is not included. The server stores friend lists
and party membership; it does not record voice. Mute Echo's game chat separately
if you want only your party to hear you. Parties support four members.

## Optional shared deployment for a free community pilot

Use a Cloudflare **Workers Free** account. Run `npm install`,
`npx wrangler login`, then `npm run deploy`. Share the resulting HTTPS URL.
No deployment has been performed as part of this prototype. The Cloudflare
account and deployment are owned by whoever runs those commands.

The Worker serves the site and a SQLite Durable Object handles signed profile
authentication, accepted friendships, invitations and WebRTC signalling.
Audio travels directly between party members over encrypted WebRTC. Their
network addresses can be visible to other members. Cloudflare's free STUN
service helps connect peers; no paid TURN relay is configured. Some restrictive
networks will fail to connect. A successful local test does not establish that
every remote connection will work.

Workers Free and Durable Objects have daily quotas; beyond the free plan limits
operations fail until reset. Free hosting is suitable for a pilot, not an
unlimited service guarantee. Do not switch to a paid plan or add a paid relay
if your requirement is a strict zero-cost service. Confirm current terms:
[Workers limits](https://developers.cloudflare.com/workers/platform/limits/),
[Durable Objects pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/),
[STUN and TURN FAQ](https://developers.cloudflare.com/realtime/turn/faq/).

## Verification

`npm test` checks profile signature validation, invitation access, room limits,
invitation expiry, owner transfer, revocation and persistence failure handling.
`node test/browser.cjs` requires Playwright and Chrome. Two isolated browsers
with fake microphones passed friendship, invitation, received audio, tab
switching, mute/deafen and leave tests. No real microphone was recorded.
The Cloudflare adapter still needs deployment testing on a real Free account.

## Tablet integration

See ../README.md for the experimental Party tablet host and its validation limits.
