# Changelog

## v1.0.6

### Fixes
- Main Versus screen (and Public Room / Ranked): the LIVE and notice (bell) buttons, and the Discord and patch notes buttons, could overlap on some phones. Those rows are now laid out from each button's real size, with gaps that follow the screen width.
- Patch Notes, Achievements, Nickname Style, Finisher and Membership windows: their close button sat slightly off the top of the screen. The windows are a little shorter now.

## v1.0.5 (2026-11-03)

### Updates
- Versus now downloads its updates from its GitHub releases (github.com/Rlaxogus123/Geode_Versus_Mode). Each release is built by GitHub from the published source code; the game checks the file's size and SHA-256 before installing it.
- Checking for an update no longer needs the Versus server or a sign-in.

### Season 2
- Plaza: the History button is now Profile (GD's profile icon) and opens your profile; your match history is in it.
- The match panel has a Profile button (labeled Profile) at its bottom left, with your LDM box right of it.
- Plaza: the notice (bell) button is hidden from when a match is found until the game starts.

### Fixes
- The result screen after a Ranked match could stop responding (CONFIRM / PLAY AGAIN stayed on SAVING... and Back did nothing) when the server's answer to saving the result was lost. The game now retries, sees when the server already saved it, and a second Back always leaves.

## v1.0.4 (2026-11-02)

### Season 2
- Plaza: the Hide button sits left of the emotes and keeps that row on screen (the emotes, the emote list and the chat); the whole row is centered at the bottom. The LIVE button is no longer in the plaza.
- Plaza: windows left open (chat, profiles, settings...) close by themselves when a match is found.
- The mini-games (Runner, Minesweeper) are always there: the Game button left of Info - not only while searching. Only a found match closes them.
- Find Match makes the panel taller only (no longer wider).
- The search board: its line shows one more tier each way (a wider range), and "+N searching" sits under the SEARCHING / PLAYING counts.
- Banned players no longer appear in a cup's Top or its player search, and no longer count in anyone's rank (as in Season 1).

### Fixes
- A battle sometimes started right from "Waiting for the other player" with no countdown (when the game heard of the start late, on a slow connection). There is now always a countdown of at least 2 seconds.
- The battle result sometimes showed nothing under one player instead of whether they had played the map before.
- The FINDING OPPONENT timer jumped back and forth by a second.
- With the QuickProfile mod installed, the game could close when choosing a Season 2 cup or coming back to it. Its features now rest while you are in a Season 2 cup (and in Versus replays), as they do for a few other mods.

## v1.0.3 (2026-11-01)

### Season 2
- Both players clearing with the same number of failed runs is now a draw - user coins no longer decide the winner.
- LIVE now lists Season 2 Ranked matches (with their cup tiers and ranks), and you can watch them there.
- The search board's line shows the other players searching in your cup.
- Plaza: the emote and chat row is centered at the bottom; the settings gear sits in the top-right corner with the notice bell next to it and LIVE under it; the Hide button at the bottom right hides the UI (it comes back when a match is found); the panel is a little smaller while it only shows FIND MATCH.
- Plaza: other players are drawn among the map's layers (behind its front decoration and your icon), and players near you fade within a shorter distance.

### Replays
- Replays record how many user coins the attempt collected, and the replay viewer shows it (Coins 1/3).

### Maps
- Maps Versus downloads for battles no longer stay in GD's Saved levels (maps you saved yourself stay).

### Fixes
- macOS: pressing other keys in the Season 2 plaza could close the game.

## v1.0.2 (2026-11-01)

### Season 2
- A draw shows its result without Play Again / Confirm; 3 seconds later both players are back in the arena for the rematch at once (no Match Found or swords again).
- Map bans: five maps are drawn and each player bans one; the map played is picked at random from the three left (they are rolled on the board). Each map shows how long it is (its play time once downloaded).
- The result panel is shorter (no RATING UPDATE line): more of the plaza shows behind it.
- The match panel is shorter: your LDM switch at its left, the lives and the map's time at its top middle, your opponent's card (and its i button) at its right. Both downloads are shown under the panel, either side of GET READY.
- Cup choice: your tier and rating sit above the cup's name, and under each card your rank in that cup (#rank / players | Top %). The cards are a little higher.
- Top 100, Top 10 and Number One achievements now also come from your rank in a Season 2 cup as soon as you reach it (in a cup with 100 or more ranked players).
- Ranked matches no longer need maps with a number of GD likes.
- Demon Cup: more varied maps - each tier's GDDL range reaches two tiers further each way, and from Extreme Demon up every extreme demon can come (only the lowest GDDL tier rises with your tier). Demons that GDDL has not rated yet can come too (by their GD difficulty). The GDDL tier box next to a map is gone.
- The Season 1 button under Ranked Match is gone (Season 1's Top 100 and records stay in profiles).

### Rooms
- Versus players' map ratings (likes / dislikes) are shown only in Ranked - no longer in public rooms or their match history.

### Fixes
- After a duel took you to the other cup, the Ranked music sometimes did not play and the camera could show past the plaza's edges.

## v1.0.1 (2026-11-01)

### Rooms
- Map Pool: Map Mode's Map Select is now Map Pool, in 1v1 and Group rooms. Apply it, then the players add maps to the room's pool (Add Map, under the pool's list), and the room plays them in order - or at random with the Random button under the list (green: on). A map that was played leaves the pool and the next one follows.
- In Map Mode the pool has its own settings: how many maps each player may add, and Filtering - the difficulties and stars, rated maps only, lengths, two-player maps and GD likes its maps must have (off: any classic map).
- The host chooses who may add maps besides themselves (the button on the challenger's card in a 1v1 room; the button at the end of each player's row in a Group room - the one at the top lets everyone). Random Map ends the pool.

### Season 2
- The Top window's player search is back (the magnifying glass at its top right): find a player of that cup by name and see their exact rank.

### Fixes
- The Season 2 cup screen no longer shows a RESULT TEST button.

## v1.0.0 (2026-10-31)

### Season 2
- Season 2 starts now and runs to the end of 2026-12-31 (KST). Ranked Match on the Versus screen opens it: choose a cup (each card shows your tier, your rating and how many players are online in it).
- Two cups, each with its own ladder: the Standard Cup (non-Demon maps, 2 to 9 stars) and the Demon Cup (Demons). Each cup has its own rating, 5 placement matches (your rating shows after them), tiers, Top 100, plaza, lounge music and online count. Everyone starts at R10000.
- Tiers come from your rating: 11 in the Standard Cup (Easy to Mystic Insane), 15 in the Demon Cup (Easy Demon to G.O.A.T). The Demon Cup's four highest tiers also need a high enough place on its ladder (G.O.A.T: one of the top 30). The Tier button shows every tier, its lives and its rating points.
- The plaza: a cup's screen is a real GD platformer map that you walk around with your own icon, with GD's controls. The players of that cup are there live, with their tier, rating and title over their heads. Emotes, emoji and quick chat show as speech bubbles; tap a player to see their profile.
- Find Match: three maps are drawn and each player bans one; the map left is played. Each tier has its own maps and lives (fewer lives on Long and XL maps). Every map has at least 5,000 GD likes and is shorter than 5 minutes.
- A draw is played again at once against the same opponent, on a new map of the next tier (its maps and lives; the top tier stays the top). Only the decisive match changes ratings.
- A found match plays out in the plaza: your opponent charges in, then you are both in the match room. After the battle you come back to the plaza.
- Duels: search a player by name and pick any tier of either cup - you play that tier's maps and lives, and no rating changes. Rematch lists your last 5 duel opponents. You can turn duel requests off and block players. Matchmaking comes before duels.
- Dodges: Season 2 has its own dodge stack, shared by both cups. Leaving a found match is +1, leaving after the map is chosen +2 (a disconnect counts as leaving). At 3 you lose two losses' worth of rating in that match's cup and can't search in either cup for 20 minutes. The stack is cleared after 5 Season 2 maps. A map that can't be downloaded is never a dodge: another map is drawn. The Dodge button shows your stack, and Log lists your recent dodges.
- Maps that players could not download, or whose song could not be downloaded, are left out of the next Season 2 searches.
- After a match, How was this map? lets you like or dislike the map.
- 22 Season 2 achievements, each with its own title.
- Your profile's season picker shows each cup's rating.

### Season 1
- Season 1 has ended. Its matchmaking and duels are closed; its Top 100 and everyone's records stay - open them with the Season 1 button under Ranked Match, or your profile's season picker.
- Everyone who played Season 1 gets a final title: "S1 R" and your last Season 1 rating, on the plate of your final Season 1 tier (its color, face and ornaments). Choose it in Titles.
- The Season 1 achievements you had reached by its end (rating, Top and tier) are all unlocked.

### Ranked
- New option in Ranked's music settings (the Music button): Always Download Song, on by default. If you turn it off, Ranked matches no longer download the map's song and sound effects automatically and you play the match without them (only what is already on your device plays). Turning it off asks you first.
- When the map's song (or some of its sounds) cannot be downloaded - the audio provider is down, a problem on your side, or the option above turned off - the match now starts anyway and is played without them, instead of waiting until the match is canceled or the map is changed.
- Ranked's music window: the song's row is on a dark plate, and Change Song / Mute are bigger.

### Rooms
- The Map Mode window is new: every difficulty in one row, each with its star buttons under it (number + star: bright when chosen, dark when not), on a darker panel. Random Map has new filters: the map's length (Tiny to XL), maps with stars only or any map, two-player maps (any, none or only), and a minimum of GD likes (type the number; empty means any). Apply is in the middle.
- A Random Map room shows a new card: a bigger title, every chosen difficulty in one row with its stars right under it, then the map lengths (a star when only maps with stars count) and the two-player and likes filters in small boxes. Group rooms show the same card.
- 1v1 rooms: the player cards are narrower and taller and the middle is wider. Your LDM box is in your own card, under your map download. In a narrow (4:3) window the emotes fit between Ready and Start.
- 1v1 rooms: the host can Ban the challenger (the button under Kick). A banned player can't join that room again while it stays open; Kick still lets them come back.
- Group rooms: the READY count is in a box at the bottom left, the emotes and chat under the player list, and the map area is bigger and wider. The Map button is now Map Mode. Each player's title is under their name (instead of Host / You / Member).
- Group rooms have an LDM box at the bottom left of the map column: your own choice for Group battles, as in 1v1 rooms.
- Group rooms: the host has a delete mark at the end of each player's row. Tapping it opens a window to choose Kick or Ban. Kick lets them join again; Ban keeps them out while the room stays open.
- Group rooms: the host has a badge next to their icon. The host can hand the room to another player: tap the small badge with an arrow on that player's icon and confirm. Everyone in the room is told who the new host is.
- Group rooms: your own row is light green.
- Random Map: Re-Roll no longer takes back Ready from the players who were ready.
- Random Map: the room no longer shows the rolled map before the roulette has stopped.
- Game Rules: the rules sit on a colored panel (gray for Attempts, light green for Percent) and the room speed on a dark one.
- Create Room: the hacks line sits on a translucent black panel.
- Room list: the panel on the right shows your icon, name and title, with Aura, Finisher, Nickname, Titles and History buttons under them (instead of My Stats). The screen is now called Public Room.
- Versus screen: the Public Room and Ranked Match cards are wider and their captions use the white font; MEMBERSHIP ACTIVE is right under VERSUS.

### Settings
- One gear at the top right of every Versus screen opens Sound and Battle Display in one window (two tabs). The rooms' UI gears and Ranked's Options gear are gone.
- Battle Display has a Layout panel: put your status, the opponent's status and the status text in any corner, or move each one with X / Y offsets and change its size. The display being laid out is marked with a yellow outline. The preview fills the whole box.

### Battles
- The mouse cursor follows GD's own "Show Cursor In-Game" option in Versus battles: with it on, the cursor stays visible all battle long, as in GD's normal play (it was always hidden). With it off, nothing changes.
- The cursor is always shown while something in a battle waits for a click - the pause menu or a question such as "Leave Versus?" (it sometimes stayed hidden), and hidden again for play afterwards.

### Replays
- A replay opens even when the map's song (or some of its sounds) cannot be downloaded: it plays without them.

### Fixes
- Ranked's penalty window (the stack next to FIND MATCH): the Korean line under the stack ran into the line above it.
- Map Mode: a filter that was off (such as the star of Rated maps only) could look on until something in the window was pressed.
- Rooms: pressing Ready in a private room sometimes failed with "Unknown room action", and sending an emote or a quick chat sometimes failed with "Ranked rooms are server controlled" (when the room changed at the same moment).

## Earlier versions

Versions before 1.0.0 were the public test releases of Versus: 1 vs 1 and Group rooms, Ranked Season 1, LIVE spectating, replays and cosmetics.
