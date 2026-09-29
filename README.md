# TagTinker pour LilyGo T-Embed CC1101

Portage de [TagTinker](https://github.com/i12bp8/TagTinker) (Flipper Zero, GPL-3.0) vers le
**LilyGo T-Embed CC1101** (ESP32-S3). Il envoie des images BMP à des étiquettes électroniques
de rayon (ESL) infrarouges **que vous possédez**. Protocole : recherches de
[furrtek](https://www.furrtek.org/?a=esl).

> **Usage** : uniquement sur vos propres étiquettes. Ne modifiez pas celles d'un magasin, ne
> perturbez pas l'infrastructure d'un tiers. Sur beaucoup d'étiquettes, retirer la pile efface
> la programmation : le tag devient inutilisable.

Le CC1101 (sous-GHz) n'est **pas** utilisé : TagTinker fonctionne en infrarouge, via la LED IR
de la carte (GPIO 2).

## Obtenir le .bin (sans rien installer)

1. Créez un dépôt GitHub (gratuit) et déposez-y **tout le contenu** de ce dossier, y compris
   le dossier caché `.github`.
2. Onglet **Actions** → workflow *Build firmware* → attendez la fin (3 à 6 min la 1re fois).
3. Ouvrez l'exécution → **Artifacts** → `tagtinker-tembed` → deux fichiers :
   - `tagtinker-tembed-launcher.bin` : **celui à copier sur la carte SD** pour votre Launcher.
   - `tagtinker-tembed-full.bin` : image complète à flasher à l'adresse `0x0`
     (esptool / outil web), seulement si vous voulez remplacer le firmware.

Alternative locale : installez PlatformIO puis `pio run` ; le binaire est
`.pio/build/t-embed-cc1101/firmware.bin`.

## Utilisation

1. Préparez une image avec l'outil web de TagTinker : <https://i12bp8.github.io/TagTinker/>
   (choisissez le profil de votre étiquette, téléchargez le BMP).
2. Copiez le BMP dans `/tagtinker/dropped/` sur la carte SD (FAT32, SanDisk ≤ 32 Go conseillé
   par LilyGo ; le dossier est créé au premier accès).
3. Sur le T-Embed :
   - **Saisir le code-barres** (1 lettre + 16 chiffres imprimés sur l'étiquette). Le modèle et
     la taille sont détectés.
   - **Envoyer une image BMP** → choisissez le fichier → page (0-7) → clic pour envoyer.
   - Placez la LED IR **tout près** de la fenêtre IR de l'étiquette. Le bouton annule l'envoi.

Commandes : **tourner** = choisir / changer une valeur, **clic** = valider, **appui long** = retour.

## Ce qui est porté / pas porté

| Porté | Non porté |
|---|---|
| Saisie du code-barres, profils d'étiquettes (table d'origine) | Étiquettes à segments (pas d'image) |
| Envoi BMP mono et couleur d'accent rouge/jaune (BMP 1 bit ou 2 plans) | SmartTag Color 2.6 (type 1626) |
| Compression auto / brute / RLE, répétition des trames | Broadcast (agit sur toutes les étiquettes à portée) |
| Redimensionnement automatique de l'image vers la taille du tag | Rendu de texte, scan NFC, plugins Wi-Fi |

## Vérifications faites (sur PC) et limites

Fait : les trames produites (ping, paramètres, données, rafraîchissement, CRC) ont été comparées
octet par octet au code d'origine du Flipper sur 6 048 combinaisons (images mono/2 plans,
plusieurs tailles, orientations de BMP, 3 modes de compression, 6 modèles, 2 pages), avec
décodage des impulsions RMT simulées. Le code C++ compile sans avertissement contre des
maquettes des API Arduino/RMT/SD/TFT.

**Non vérifié sur le vrai matériel** : compilation PlatformIO complète, affichage, encodeur, SD,
et surtout l'émission IR réelle.

## Si ça ne marche pas

- **Écran décalé / couleurs inversées** : dans `platformio.ini`, retirez `-DTFT_INVERSION_ON=1`
  ou `-DCGRAM_OFFSET=1`. L'orientation se règle dans *Réglages*.
- **Encodeur à l'envers** : *Réglages → Sens encodeur*. S'il saute plusieurs crans, changez
  `LatchMode::TWO03` en `FOUR3` dans `main.cpp`.
- **L'étiquette ne réagit pas** :
  1. *Réglages → Test LED IR* : filmez la LED avec un téléphone, elle doit apparaître allumée.
     (Ce test prouve que la LED s'allume, pas que la modulation est fidèle.)
  2. Certains T-Embed CC1101 Plus ont une émission IR défaillante (voir l'issue #64 du dépôt
     LilyGo). Testez l'exemple `infrared_send_test` de LilyGo.
  3. **Point faible connu** : les étiquettes ESL attendent une porteuse d'environ **1,25 MHz**.
     La LED du T-Embed est prévue pour des télécommandes à 38 kHz ; son étage de commande peut
     être trop lent ou trop faible à cette fréquence, et sa portée est courte. Collez la LED à
     l'étiquette, ou envisagez une LED IR externe rapide (GPIO libre à modifier dans
     `board_pins.h`).
- **Erreur « Image trop lourde »** : en mode brut, une image 2 plans 800x480 dépasse les 65 535
  octets du champ « taille » du protocole. Utilisez la compression auto ou RLE. (Le code d'origine
  tronque silencieusement cette valeur ; ce portage refuse plutôt que d'envoyer une trame
  corrompue.)

## Licence et crédits

GPL-3.0 (voir `LICENSE`). Code protocole (`src/tagtinker_proto.*`) repris de TagTinker
(i12bp8). Recherche sur le protocole : furrtek. Brochage : dépôt officiel LilyGo T-Embed-CC1101.
