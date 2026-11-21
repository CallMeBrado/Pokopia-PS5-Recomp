#include "save_store.h"
int main(void)
{
    char path[HZN_SAVE_PATH_MAX];
    return hzn_save_ensure_account("/data/pokopia/save", 1, 2) ||
           hzn_save_open_account("/data/pokopia/save", 1, 2, path);
}
