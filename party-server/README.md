# Watch party relay

Tiny Cloudflare Worker that lets Echo Arcade players watch REELS / TIKTOK together. The host's ArcadeHost sends the
link and play time of what they are watching (about one small message a second); guests' ArcadeHost opens the same
video on their own posters. No video passes through the relay. Runs on Cloudflare's free plan.

Deploy once (needs Node.js and a free Cloudflare account):

```
npx wrangler login
npx wrangler deploy
```

Wrangler prints the address, e.g. `https://echo-watch-party.<you>.workers.dev`. Put it in `EchoArcade\arcade.ini`:

```
[host]
party_server=https://echo-watch-party.<you>.workers.dev
```

Test locally with `npx wrangler dev` and `party_server=http://127.0.0.1:8787`.
