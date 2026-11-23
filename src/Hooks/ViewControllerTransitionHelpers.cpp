#include "hooking.hpp"
#include "logging.hpp"

#include "GlobalNamespace/JoiningLobbyViewController.hpp"
#include "HMUI/ViewControllerTransitionHelpers.hpp"
#include "HMUI/ViewController.hpp"

MAKE_AUTO_HOOK_MATCH(ViewControllerTransitionHelpers_DoPresentTransition, &::HMUI::ViewControllerTransitionHelpers::DoPresentTransition, System::Collections::IEnumerator*, HMUI::ViewController* toPresentViewController, HMUI::ViewController* toDismissViewController, HMUI::ViewController::AnimationDirection animationDirection, float moveOffsetMultiplier) {
    if (i2c::try_cast<GlobalNamespace::JoiningLobbyViewController*>(toDismissViewController))
        animationDirection = HMUI::ViewController::AnimationDirection::Vertical;
    return ViewControllerTransitionHelpers_DoPresentTransition(toPresentViewController, toDismissViewController, animationDirection, moveOffsetMultiplier);
}
