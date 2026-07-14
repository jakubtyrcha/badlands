# Danger estimate

Each character will have a danger estimate, used to:
- pick enemies
- decide when to fight/flee
- balance the numbers

Let's say 0-20+
Roughly creature of level X should win duels against most creatures with lower level and lose most against creatures from levels above
This can be impresise due to RNG, skill usage, some special skills, weaknesses and strengths (maybe one creature is fire based and the other is fire resistant, they would NOT have equal chances despire same danger level, but that would even out against duels vs other creatures with similar levels), on average it should be statistically represetnative.
Specifically creature of a few levels above should win against 95%+ creatures of level X.

Kinda rough values:
rat would be 0.25 (is a danger for peasants, tax collectors, can overwhelm apprentices, hunters and grave robbers at low levels, mercenary should easily handle a few from the get go)
guard would be 3
goblin would be 1 (can handle apprentice and is dangerous to grave robber IF gets to melee)
troll would be 8

# Power curve

Classes will have a power curve 
Levels 1-20+ 
0-8 early game
9-16 mid game
17+ late game

Example:
- grave robber => balanced, can handle themselves, a bit unreliable due to their character but high dps mid/late game with high utility skills (posions, bloodletting), has solid attack and can evade, but low armour, has low range ranged attack (hand crossbow), low cooldown attack with high chance for critical damage
  - l1 at d1, l9 - d9, l16 - d18, l20 - d20.5
- hunter => balanced, most powerful at mid levels due to high utility skills, they become a bit meh later, quite okay from beginning due to ranged abilities, has strong range attacks and reasonable melee, below avg defence, below avg armour
 - l1 at d1.5, level 9 would be d9, level 16 would be d17 and l20 would be d20
- mercenary => balanced but flatter, quite powerful at the start due to survivability, falls flat later especially late game due to mediocre utility and dps, mostly useful for taking hits, strong armour, defence and solid damage at the start
  - l1 at d2.5, levle 16 would be d15, l20 would be d17
- apprentice => very flat at the start with rapid tail at high levels
  - l1 at d0.5, level 9 would be 5, level 16 would be 16 and level 20 would be 25


mercenary and hunter should be the defensive staple
grave robber is solid due to dps and other utility 
apprentice is a high risk high reward, very fragile

# MVP
Just do 1-8 early game levelling

