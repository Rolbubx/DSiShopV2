// État des entrées pour une frame (indépendant de libnds)
#ifndef INPUT_H__
#define INPUT_H__

#include <stdbool.h>
#include <stdint.h>

// Mêmes valeurs que les KEY_* de libnds, on peut donc passer keysDown() tel quel
#define BTN_A      (1u << 0)
#define BTN_B      (1u << 1)
#define BTN_SELECT (1u << 2)
#define BTN_START  (1u << 3)
#define BTN_RIGHT  (1u << 4)
#define BTN_LEFT   (1u << 5)
#define BTN_UP     (1u << 6)
#define BTN_DOWN   (1u << 7)
#define BTN_R      (1u << 8)
#define BTN_L      (1u << 9)
#define BTN_X      (1u << 10)
#define BTN_Y      (1u << 11)
#define BTN_TOUCH  (1u << 12)

typedef struct {
    uint32_t down;    // Pressés pendant cette frame
    uint32_t held;    // Maintenus
    uint32_t up;      // Relâchés pendant cette frame
    uint32_t repeat;  // Comme down, avec auto-répétition
    bool touching;
    int tx, ty;       // Dernière position du stylet (valide même au relâchement)
} input_t;

#endif // INPUT_H__
