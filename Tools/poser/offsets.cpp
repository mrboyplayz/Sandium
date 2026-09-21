#include "structs/ItemType.hpp"
#include <cstdio>
int main() {
    printf("sizeof=%zu\n", sizeof(structs::ItemType));
    printf("handCount=%zu\n", offsetof(structs::ItemType, handCount));
    printf("rightHandOffset=%zu\n", offsetof(structs::ItemType, rightHandOffset));
    printf("leftHandOffset=%zu\n", offsetof(structs::ItemType, leftHandOffset));
    printf("useAlternativeAim=%zu\n", offsetof(structs::ItemType, useAlternativeAim));
    printf("primaryGripStiffness=%zu\n", offsetof(structs::ItemType, primaryGripStiffness));
    printf("primaryGripRotation=%zu\n", offsetof(structs::ItemType, primaryGripRotation));
    printf("secondaryGripStiffness=%zu\n", offsetof(structs::ItemType, secondaryGripStiffness));
    printf("secondaryGripRotation=%zu\n", offsetof(structs::ItemType, secondaryGripRotation));
    printf("gunHoldPosition=%zu\n", offsetof(structs::ItemType, gunHoldPosition));
    printf("name=%zu\n", offsetof(structs::ItemType, name));
    return 0;
}
