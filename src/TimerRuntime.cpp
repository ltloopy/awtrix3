#include "TimerRuntime.h"

namespace TimerRuntime
{
    // Running with zero remaining is the Finished transition. Ported straight from
    // TimerManager::enterFinished, in the SAME order the imperative code emitted:
    // publishState, publishRemaining, then the three gated effects (switch-to-app,
    // brightness restore, end tone). Keeping the order byte-identical is what lets
    // the adapter stay behaviourally indistinguishable on device (issue #178).
    static std::vector<Effect> enterFinished(const Inputs &in)
    {
        std::vector<Effect> fx;
        fx.push_back({EffectKind::PublishState});
        fx.push_back({EffectKind::PublishRemaining});
        if (in.navigationFree)
            fx.push_back({EffectKind::SwitchToTimerApp});
        if (in.matrixOff)
        {
            Effect e{EffectKind::SetBrightness};
            e.brightness = in.brightness;
            fx.push_back(e);
        }
        if (in.playEndTone)
        {
            Effect e{EffectKind::PlayTone};
            e.tone = Tone::End;
            fx.push_back(e);
        }
        return fx;
    }

    std::vector<Effect> step(const State &state, unsigned long nowMs, const Inputs &in)
    {
        (void)nowMs;   // reserved: the Finished/ReAlert timing slices key on it
        if (state.phase == TimerState::Running && state.remainingSec == 0)
            return enterFinished(in);
        return {};
    }
}
