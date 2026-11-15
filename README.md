# DSiShopV2

Boutique homebrew pour Nintendo DSi, construite avec [BlocksDS](https://blocksds.skylyrac.net/).

- `source/` — l'application DSi (ARM9)
- `catalog-server/` — serveur de catalogue (Python, bibliothèque standard uniquement)

## Compiler

### Installer la chaîne d'outils (Linux, une seule fois)

```sh
sudo mkdir -p /opt/wonderful
curl -sSL https://wonderful.asie.pl/bootstrap/wf-bootstrap-x86_64.tar.gz | sudo tar xz -C /opt/wonderful
export PATH=/opt/wonderful/bin:$PATH

wf-pacman -Syu --noconfirm wf-tools
wf-config repo enable blocksds
wf-pacman -Syu --noconfirm
wf-pacman -S --noconfirm blocksds-toolchain \
    toolchain-gcc-arm-none-eabi-zlib blocksds-mbedtls blocksds-libcurl
sudo ln -s /opt/wonderful/thirdparty/blocksds /opt/blocksds
```

### Avec Docker (sans rien installer)

```sh
docker run --rm -v "$PWD":/work -w /work skylyrac/blocksds:slim-latest sh -c \
  "wf-pacman -Sy --noconfirm toolchain-gcc-arm-none-eabi-zlib blocksds-mbedtls blocksds-libcurl && make"
```

### Construire la ROM

```sh
make                                              # produit DSiShop.nds
make REMOTE_SERVER=https://dsishop.mondomaine.fr  # avec l'adresse du serveur Remote
make clean
```

Chaque push déclenche aussi la compilation sur GitHub Actions
(`.github/workflows/build.yml`) ; la ROM est disponible dans les artefacts du run.

## Serveur de catalogue

```sh
python3 catalog-server/catalog.py
```

Variables d'environnement optionnelles : `GITHUB_TOKEN`, `CATALOG_FILE`, `PORT`
(voir l'en-tête de `catalog-server/catalog.py`).
