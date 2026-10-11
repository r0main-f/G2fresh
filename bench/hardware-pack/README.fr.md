# Pack de test G2fresh pour ton Nord Modular G2

*English version: [README.md](README.md). Pour imprimer : README.fr.html.*

Merci pour ton aide ! G2fresh est un nouvel éditeur pour le G2, qui sait aussi jouer les patchs sans G2. Certaines choses ne se vérifient que sur un vrai. Compte une vingtaine de minutes : tu charges quelques petits patchs, tu les enregistres, tu réponds à quelques questions et tu renvoies le tout.

**Il te faut :** un G2, un G2X ou un G2 Engine ; l'éditeur de Clavia sur un ordinateur qui le fait encore tourner (pas besoin de G2fresh) ; une carte son avec deux entrées ligne (quatre, c'est mieux) et de quoi enregistrer des fichiers WAV.

**Dans ce pack :** `patches/` (B01 à B14 ; ils jouent tout seuls, sans clavier), `B10 Stimulus.wav` (un signal de test pour l'étape 3), `form.fr.txt` (tes réponses ; en anglais : `form.txt`).

Rien n'est mémorisé dans ton G2 tant que tu n'appuies pas sur Store. Charger un patch remplace celui du slot (B08 remplace toute la performance) : sauvegarde d'abord ton travail en cours.

## 1. Installation

1. Branche les sorties OUT 1 et OUT 2 du G2 sur deux entrées ligne de ta carte son, et aussi OUT 3 et OUT 4 si tu as quatre entrées libres. Des câbles directs : pas de console, pas d'effets, pas d'égalisation.
2. Tourne le potentiomètre **Master Level** du G2 à fond.
3. Sur la carte son, utilise des entrées ligne. Si elles ont un sélecteur de niveau (+4 dBu / -10 dBV), note sa position.
4. Baisse tes enceintes : B02 et B06 sont forts et agressifs.
5. Charge **B02 Level Steps** (l'étape 2 explique comment) et regarde tes vumètres d'entrée pendant 15 secondes. Règle le gain d'entrée pour que le moment le plus fort reste sous le haut du vumètre ; en général, le minimum convient. **Ensuite, ne touche plus au gain**, et note sa position dans le formulaire.
6. Règle ton enregistreur sur **WAV, 24 bits, 96 kHz** (48 kHz si 96 n'est pas possible). Pas de normalisation, pas de plug-in, aucun traitement.

## 2. Enregistrer les patchs

Pour charger un patch : dans l'éditeur de Clavia, choisis **File > Open** et sélectionne le fichier (ou glisse le fichier sur un slot). L'écran du G2 affiche le nom du patch, qui se met à jouer. Tu peux aussi copier les patchs dans une banque avec l'éditeur et les charger depuis la façade du G2.

Pour chaque ligne du tableau : charge le patch, attends deux secondes, enregistre pendant la durée indiquée et sauvegarde l'enregistrement sous le nom indiqué. Un fichier stéréo avec OUT 1 à gauche et OUT 2 à droite, c'est ce qu'il nous faut.

| Charger | Enregistrer | Sauvegarder sous | Tu dois entendre |
|---|---|---|---|
| B01 Level Ref | 30 s | B01 Level Ref.wav | un son continu |
| B02 Level Steps | 40 s | B02 Level Steps.wav | un son plus fort à chaque seconde, saturé à la fin |
| B03 Output Map | 20 s | B03 Output Map.wav | un accord : une note par sortie |
| B04 Pad | 30 s | B04 Pad.wav | un son à deux niveaux, puis un silence |
| B05 White Noise | 30 s | B05 White Noise.wav | du souffle |
| B06 Bright Saw | 30 s | B06 Bright Saw.wav | des notes qui grésillent, de plus en plus aiguës |
| B07 Silence | 15 s | B07 Silence.wav | rien |
| B08 Clock 120 | 60 s | B08 Clock 120.wav | des clics, deux par seconde de chaque côté |
| B09a Saw Filter | 20 s | B09a Saw Filter.wav | une courte phrase de basse |
| B09b FM Phrase | 20 s | B09b FM Phrase.wav | une courte phrase au son de cloche |
| B09c Drum | 20 s | B09c Drum.wav | un motif de batterie |
| B09d Rev Delay | 30 s | B09d Rev Delay.wav | un clic avec des échos, puis un clic avec de la réverbération |

- **B03** utilise les quatre sorties. Avec quatre entrées, sauvegarde un fichier à quatre canaux, ou deux fichiers stéréo nommés `B03 Output Map 1-2.wav` et `B03 Output Map 3-4.wav`. Avec deux entrées, enregistre seulement OUT 1 et 2.
- **B08** est une performance (`B08 Clock 120.prf2`) : elle s'ouvre de la même façon.

## 3. L'entrée audio (B10)

1. Charge **B10 Audio In**. Il envoie l'entrée IN 1 du G2 directement vers OUT 1.
2. Branche la sortie 1 de ta carte son sur l'entrée **IN 1** du G2. Laisse OUT 1 du G2 sur l'entrée 1 de la carte son.
3. Si tu peux, branche aussi la sortie 2 de ta carte son directement sur son entrée 2 (débranche OUT 2 du G2 pour ça). Ce câble est notre référence : il nous dit à quel niveau ta carte son joue le fichier et combien de temps elle met.
4. Place `B10 Stimulus.wav` sur une piste qui sort sur les sorties 1 et 2, fader à 0 dB. Si la carte son a un bouton de volume pour ces sorties, monte-le à fond (casque retiré) : les sons les plus forts doivent atteindre la limite de l'entrée du G2. Coupe le monitoring d'entrée, sinon ça va hurler.
5. Enregistre les entrées 1 et 2 pendant la lecture du fichier (14 secondes). Sauvegarde sous `B10 Audio In.wav`.

## 4. Quelques points à observer

Note les réponses dans `form.fr.txt` au fur et à mesure.

1. Éteins puis rallume le G2. L'écran affiche la version de l'OS pendant une seconde (Q04). Avant de charger quoi que ce soit, appuie sur **Patch Settings** si son voyant est éteint : le petit écran au-dessus des potentiomètres 1 et 2 affiche un tempo et, en dessous, Run ou Stop (Q15).
2. Charge **B11**, **B12** et **B13** l'un après l'autre. Chacun demande 32 voix et est trop lourd pour toutes les obtenir. Avec le voyant Patch Settings allumé, l'écran au-dessus du potentiomètre 2 affiche quelque chose comme `03 (32)` : note exactement ce que tu vois (Q12 à Q14). Sur un G2 Engine, l'éditeur affiche le nombre de voix dans sa barre d'outils. Ces trois patchs restent muets tant que tu ne joues pas une touche.
3. Modèles à clavier : charge **B14 Wheels**. Il tient une note. Pousse le pitch stick vers la droite : la note monte ou descend (Q16) ? Sur un G2X, tourne la G.Wheel 1 en l'éloignant de toi : elle monte ou descend (Q17) ? Tourne la G.Wheel 2 en l'éloignant de toi : une deuxième note, plus aiguë, apparaît ou disparaît (Q18) ?
4. Appuie sur **System**, puis sur la touche Down jusqu'à ce que l'écran affiche `Memory Prot`, et tourne la molette sur On. Appuie sur **Store** une fois, une seule. Qu'affiche l'écran (Q19) ? S'il propose de mémoriser, appuie deux fois sur Patch pour sortir sans rien mémoriser. Remets ensuite Memory Protect comme il était.

## 5. Renvoyer le tout

Complète le reste de `form.fr.txt`, mets-le dans un dossier avec tes fichiers WAV, compresse le dossier (zip) et envoie-le à la personne qui t'a donné ce pack. Merci !

## En option : essayer G2fresh avec ton G2

*Il faut G2fresh v0.10.0 ou plus récent. Sous Windows, G2fresh a besoin du pilote WinUSB pour le G2 (voir le README de G2fresh).*

1. Quitte l'éditeur de Clavia : un seul programme à la fois peut parler au G2.
2. Lance G2fresh et choisis **Synth > Connect to G2 (USB)**. La barre d'état affiche la version du G2.
3. Choisis **Synth > Get Patch from Slot > A**. Le patch du slot A doit apparaître.
4. Tourne un potentiomètre dans G2fresh et écoute ; puis tourne un potentiomètre sur le G2 et regarde G2fresh suivre.
5. Choisis **Synth > Send Patch to Slot > B**.
6. Ouvre **Synth > Synth Memory (Banks)...**, explore, et mémorise un patch dans un emplacement vide.
7. Quoi qu'il se soit passé, choisis **Synth > Show USB Log** et envoie-nous `usb.log` (et `usb.log.1` s'il existe) avec quelques mots sur ce qui a marché ou non. Le journal ne contient que le trafic USB, sans le contenu de tes patchs.
