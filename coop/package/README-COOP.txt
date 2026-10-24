GoldenEye 007 story co-op (private build)
=========================================

Two players play the solo missions together over Steam. Nobody has to open
ports or type IP addresses.

You need
--------
- Steam running and logged in on both PCs. The two accounts must be Steam
  friends. Nobody has to own anything: the game connects as Valve's free
  "Spacewar" test app, so Steam will show you both as "playing Spacewar".
- Your own GoldenEye 007 (USA) cart image in the Data folder (see
  Data\README if present). The image is not included.

Playing
-------
1. The host double-clicks "Host co-op.bat", then picks a folder, a mission and a
   difficulty as usual.
2. The friend double-clicks "Join co-op.bat" and waits on the title screen. Their
   game finds the host by itself and follows them into the mission. It does
   the same for every later mission.
   (Accepting a Steam invite from the host also works if the game is already
   running.)

Rules
-----
- The host's game runs the guards and the level. Both players see the same
  guards, doors, alarms, destroyed objects and pickups.
- Objectives are shared. Whoever completes the mission finishes it for both.
- When you die, you come back next to your partner once they have been out
  of combat for 4 seconds, and after 20 seconds at most. If you are both dead
  at the same time, the mission fails.

If something goes wrong
-----------------------
Press ~ (tilde) in game to open the console. The co-op lines start with
"ge: steam:" and "ge: coop:". Send those lines along with what happened.

Known gaps in this build
------------------------
- Cutscene cameras follow the host's script; tell me if one looks wrong.
