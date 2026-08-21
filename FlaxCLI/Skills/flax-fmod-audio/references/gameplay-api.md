# Gameplay audio API authoring

## Current public surface

The implemented managed API is `AudioEventSystem`, not an `Audio.Play` facade. Treat proposals written as `Audio.Play`, `Audio.PlayAt`, `Audio.Stop`, or `Audio.SetParameter` as the intended ergonomic shape unless equivalent facade methods have actually been added to `Audio`.

The owner-managed persistent API is:

```csharp
AudioEventHandle AudioEventSystem.Play(AudioEvent audioEvent, Actor owner);
AudioEventHandle AudioEventSystem.Play(
    AudioEvent audioEvent,
    Actor owner,
    AudioParameterValue[] initialParameters);

AudioEventHandle AudioEventSystem.PlayAt(AudioEvent audioEvent, Vector3 position);
AudioEventHandle AudioEventSystem.PlayAt(
    AudioEvent audioEvent,
    Vector3 position,
    AudioParameterValue[] initialParameters);

bool AudioEventSystem.Stop(
    AudioEvent audioEvent,
    Actor owner,
    AudioStopMode mode = AudioStopMode.AllowFadeOut);

bool AudioEventSystem.SetParameter(
    AudioEvent audioEvent,
    Actor owner,
    AudioParameterId parameter,
    float value,
    bool ignoreSeekSpeed = false);
```

Handle-based control remains available for pause, key-off, stop, release, spatial attributes, masks, parameters, volume, pitch, timeline position, and readback.

## Ownership semantics

`Play(audioEvent, owner)` identifies one persistent instance by `(event asset ID, owner Actor ID)`:

- The first call creates and starts an instance.
- A later call for the same pair retriggers the tracked instance when valid.
- The instance follows the owner's final transform and inferred velocity without requiring an `AudioEmitter` Actor.
- `Stop(audioEvent, owner, mode)` stops and releases that tracked instance.
- Owner destruction or leaving play stops and releases it.
- FMOD still owns real/virtual voice selection and authored polyphony.

Use the returned handle when one caller needs direct control or readback. Use the semantic `(event, owner)` overload when several systems need to address the same persistent sound without sharing a handle.

`PlayAt` creates a tracked fixed-position instance. It does not follow an Actor. Naturally completed tracked instances are released automatically.

## Typed example

```csharp
public JsonAssetReference<AudioEvent> MotorEvent;

private readonly AudioParameterId _rpm = new AudioParameterId { Name = "RPM" };
private readonly AudioParameterId _load = new AudioParameterId { Name = "Load" };
private AudioEventHandle _motor;

private void StartMotor(float rpm, float load)
{
    var audioEvent = MotorEvent.Instance;
    if (audioEvent == null)
        return;

    var initial = new[]
    {
        new AudioParameterValue { Id = _rpm, Value = rpm },
        new AudioParameterValue { Id = _load, Value = load },
    };
    _motor = AudioEventSystem.Play(audioEvent, Actor, initial);
}

private void SetRpm(float value)
{
    var audioEvent = MotorEvent.Instance;
    if (audioEvent != null)
        AudioEventSystem.SetParameter(audioEvent, Actor, _rpm, value, false);
}

private void StopMotor()
{
    var audioEvent = MotorEvent.Instance;
    if (audioEvent != null)
        AudioEventSystem.Stop(audioEvent, Actor, AudioStopMode.AllowFadeOut);
    _motor = default;
}
```

Initial parameters are applied before `start()`. This is required when an authored parameter controls whether an instrument produces a voice or begins silent/virtualized.

## Choose the correct playback model

- Use `AudioEmitter` for inspector-authored scene playback, activation rules, occlusion, listener masks, and component-local parameters.
- Use owner-managed `Play` for persistent gameplay sounds attached to an existing Actor without adding an emitter component.
- Use `PlayAt` for a controllable fixed-position event.
- Use `PlayOneShotFromAsset` for fire-and-forget typed playback.
- Use low-level `CreateInstance` only when custom lifetime, callbacks, masks, or staged configuration requires it; pair every retained handle with a deliberate stop/release policy.

Prefer stable `AudioParameterId` values resolved from typed event metadata. String names are acceptable during authoring and migration, but repeated runtime resolution and raw event paths should not become the primary API.

Validate persistent behavior with `QueryInstance` and `GetParameter`, and use `CaptureDiagnostics` or the CLI audio commands to prove the instance reaches measured output.
