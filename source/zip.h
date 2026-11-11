// Extraction d'une ROM (.nds / .dsi) depuis une archive ZIP, ZIP64 compris
// (archives de plus de 4 Go), lue à travers un fichier découpé (splitfile).
// Portable (stdio + zlib) : testé sur PC dans tools/test.
#ifndef ZIP_H__
#define ZIP_H__

#include <stddef.h>
#include <stdint.h>

#include "splitfile.h"

typedef enum {
    ZIP_OK = 0,
    ZIP_ERR_FORMAT,     // Pas un zip / répertoire central introuvable
    ZIP_ERR_NO_ROM,     // Aucun .nds / .dsi dans l'archive
    ZIP_ERR_METHOD,     // Compression non supportée (autre que stored/deflate)
    ZIP_ERR_TOO_BIG,    // ROM de plus de 4 Go : impossible sur FAT32
    ZIP_ERR_CORRUPT,    // Données ou CRC invalides
    ZIP_ERR_WRITE,      // Écriture impossible (SD pleine ?)
    ZIP_ERR_MEMORY,
    ZIP_ERR_CANCELLED,
} zip_err_t;

// Appelé régulièrement pendant l'extraction. Renvoyer non nul pour annuler.
typedef int (*zip_progress_t)(void *userdata, uint64_t done, uint64_t total);

// Cherche la ROM (.nds en priorité, sinon .dsi) et l'écrit dans out_path.
// Le nom de la ROM dans l'archive (sans dossier) est copié dans name_out.
zip_err_t zip_extract_rom(sfile_t *zip, const char *out_path,
                          char *name_out, size_t name_size,
                          zip_progress_t progress, void *userdata);

const char *zip_strerror(zip_err_t err);

#endif // ZIP_H__
