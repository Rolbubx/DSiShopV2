# DSi Shop Remote — serveur

Petit serveur Node.js (aucune dépendance) qui fait le lien entre ta DSi et ton
téléphone :

1. Sur la DSi, **Remote** affiche un QR code (code d'appairage à usage unique, 10 min).
2. Tu le scannes : le site s'ouvre et te demande de **te connecter avec Google**.
3. La DSi est liée à ton compte. Depuis le téléphone tu peux alors :
   - installer un jeu (lien GitHub ou lien direct),
   - **chercher des projets sur GitHub** et installer un fichier de leur dernière release,
   - voir les jeux installés et les **désinstaller**,
   - suivre la progression, annuler, redémarrer la DSi.

La DSi récupère les commandes en *long polling* HTTPS (elle n'a pas besoin d'être
joignable depuis Internet).

## Lancer en local (test)

```sh
cd remote-server
DEV_LOGIN=1 PUBLIC_URL=http://IP-DE-TON-PC:8080 node server.js
```

`DEV_LOGIN=1` ajoute un formulaire de connexion **sans Google**, pour tester. Ne
l'active jamais sur un serveur public. Sur la DSi, mets `http://IP-DE-TON-PC:8080`
dans `sd:/_dsishop/server.txt`.

Tests : `npm test` (ou `node --test test.js`).

## Mettre en ligne

Il faut une adresse **HTTPS** publique (Google l'exige). N'importe quel hébergeur
Node.js ou Docker convient (Render, Fly.io, Railway, un VPS avec Caddy…).

### 1. Créer l'identifiant Google

1. Va sur [console.cloud.google.com](https://console.cloud.google.com/), crée un projet.
2. **API et services → Écran de consentement OAuth** : type « Externe », renseigne le
   nom de l'appli et ton e-mail (pas besoin de scopes particuliers).
3. **API et services → Identifiants → Créer des identifiants → ID client OAuth** :
   - type : **Application Web**,
   - **Origines JavaScript autorisées** : l'adresse de ton site, ex.
     `https://dsishop.mondomaine.fr`.
4. Copie l'**ID client** (`…apps.googleusercontent.com`).

### 2. Variables d'environnement

| Variable | Rôle |
|---|---|
| `GOOGLE_CLIENT_ID` | ID client de l'étape 1 (obligatoire) |
| `PUBLIC_URL` | URL publique, ex. `https://dsishop.mondomaine.fr` (utilisée dans le QR code) |
| `PORT` | Port d'écoute (défaut 8080) |
| `DATA_FILE` | Fichier de sauvegarde (défaut `./data.json`, `/data/data.json` en Docker) |

### 3. Docker

```sh
docker build -t dsishop-remote remote-server
docker run -d -p 8080:8080 -v dsishop-data:/data \
  -e GOOGLE_CLIENT_ID=xxx.apps.googleusercontent.com \
  -e PUBLIC_URL=https://dsishop.mondomaine.fr \
  dsishop-remote
```

Mets un reverse proxy HTTPS devant (Caddy : `dsishop.mondomaine.fr { reverse_proxy localhost:8080 }`).

### 4. Indiquer le serveur à la DSi

Au choix :
- compile DSi Shop avec `make REMOTE_SERVER=https://dsishop.mondomaine.fr`
  (fais `make clean` avant si tu changes l'adresse),
- ou crée `sd:/_dsishop/server.txt` contenant l'adresse.

## Sécurité

- Connexion vérifiée côté serveur : signature RS256 du jeton Google (clés publiques
  de Google), émetteur, audience (`GOOGLE_CLIENT_ID`) et expiration.
- Session par cookie `HttpOnly`, `SameSite=Lax` (`Secure` en HTTPS). Toutes les
  requêtes d'écriture exigent l'en-tête `X-DSiShop` (protection CSRF).
- Jeton de la DSi : 256 bits aléatoires, stocké **haché** côté serveur.
- Code d'appairage : 8 caractères, 10 minutes, usage unique. Il faut voir l'écran de
  la DSi pour l'obtenir.
- Une DSi n'est visible et pilotable que par le compte qui l'a associée.
- Commandes validées : liens sans espace (300 caractères max), désinstallation
  limitée aux noms de fichiers `.nds` / `.dsi` sans dossier. La DSi revérifie de son
  côté.
- Limitation de débit sur l'enregistrement, la connexion et l'appairage.
- En-têtes `Content-Security-Policy` stricts sur le site.
