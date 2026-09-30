# PKGi Remastered — PS3

**PKGi Remastered** est un fork modernisé de [pkgi-ps3](https://github.com/bucanero/pkgi-ps3) :
installez des PKGs directement depuis le XMB, avec les bases NPS, une vraie fiche
produit et une couche manette corrigée.

![PKGi Remastered](pkgfiles/ICON0.PNG)

## Nouveautés de ce fork

### 🎮 Couche manette refondue
Le bug bien connu « aucune touche ne répond avec certaines manettes » est corrigé à la
racine (`source/ps3_input.c`) :
- **tous les ports** sont scannés (avant : port 0 codé en dur) ;
- chaque trame pad est **validée** (`len > 0` + marqueur libpad `0x7`, packing standard
  ou alterné) — les manettes tierces au format court fonctionnent ;
- en cas de lecture invalide, l'état connu est conservé, sans événements fantômes ;
- **hystérésis analogique** (seuils engage/release) : plus de navigation qui scintille ;
- **répétition temporelle** (350 ms de délai, 110 ms de période) au lieu du compteur
  de frames.

Mapping : D-pad/stick = navigation · ✕/○ = valider/retour (selon réglage console) ·
△ = menu · □ = action secondaire · L1/R1/L2/R2 = pages/catégories · START = menu ·
SELECT = informations.

### ⚙️ config.txt modernisé (compatibilité legacy conservée)
Sans `config.txt`, l'app démarre avec les **URLs NPS par défaut**
(games/dlcs/themes/avatars/demos), téléchargement en arrière-plan activé, musique coupée.

Clé nouvelle : `db_format_<tag> = nps | pkgi` (par type de contenu).

**Priorité des formats de base de données, documentée :**
1. `dbformat.txt` legacy (inchangé, prioritaire s'il existe) ;
2. `db_format_*` dans `config.txt` ;
3. auto-détection TSV NPS (10 colonnes, délimiteur tab) ;
4. format historique 8 colonnes.

Le format NPS supporte : `Title ID | Region | name | url | rap | contentid |
Last Modification Date | Download .RAP file | size | checksum`.

### 📂 Charger une configuration (menu △)
L'entrée **« Charger une configuration »** charge `<config>/profiles/<nom>.txt`
(`nps`, `custom`, `homebrew` reconnus). Le chargement est **transactionnel** :
clé ou valeur invalide → erreur affichée, configuration précédente conservée.
Jamais de crash.

### 🖼️ Fiche produit enrichie
La page Informations affiche cover (cache TMDB existant, fallback « Pas de
jaquette » propre), overlay sombre pour la lisibilité, et toutes les données
disponibles : Title ID, région, taille localisée, date de mise à jour, RAP/SHA256.
Seuls les champs réellement présents dans la base sont affichés — rien n'est inventé.

### 🇫🇷 Français complet
`LANG/fr.po` étendu et corrigé (menus, erreurs, téléchargements, configuration).

## Build

Identique au projet d'origine (toolchain PSL1GHT) :

```sh
make            # EBOOT.BIN
make pkg        # pkgi-ps3.pkg
```

La CI GitHub Actions (`.github/workflows/build.yml`) construit le PKG à chaque
push et le publie comme artefact ; les tags produisent une release.

## Crédits

- [bucanero](https://github.com/bucanero) — auteur de pkgi-ps3
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT), ya2d, mini18n — SDK et bibliothèques

Licence : voir [LICENSE](LICENSE) (GPL).
