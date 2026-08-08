// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using Newtonsoft.Json.Linq;

namespace FlaxEngine
{
    /// <summary>
    /// Captures a small, machine-readable snapshot of the runtime input layer.
    /// This is intentionally independent of a particular player script so the CLI
    /// can distinguish device, mapping, and state problems before inspecting code.
    /// </summary>
    public static class CliInputProbe
    {
        /// <summary>
        /// Captures input devices, mappings, current state, and optional named samples.
        /// </summary>
        /// <param name="arguments">Optional key, axis, and action query arguments.</param>
        /// <returns>The diagnostic snapshot.</returns>
        public static object Capture(JObject arguments)
        {
            arguments ??= new JObject();

            var requestedKeys = ReadValues(arguments, "key");
            var requestedAxes = ReadValues(arguments, "axis");
            var requestedActions = ReadValues(arguments, "action");
            var actionMappings = Input.ActionMappings ?? Array.Empty<ActionConfig>();
            var axisMappings = Input.AxisMappings ?? Array.Empty<AxisConfig>();
            var actionNames = new List<string>();
            var axisNames = new List<string>();
            var actionDetails = new List<object>();
            var axisDetails = new List<object>();

            foreach (var mapping in actionMappings)
            {
                var name = mapping.Name ?? string.Empty;
                actionNames.Add(name);
                actionDetails.Add(new
                {
                    name,
                    mode = mapping.Mode.ToString(),
                    key = mapping.Key.ToString(),
                    mouseButton = mapping.MouseButton.ToString(),
                    gamepad = mapping.Gamepad.ToString(),
                    gamepadButton = mapping.GamepadButton.ToString(),
                    deadZone = mapping.DeadZone,
                });
            }

            foreach (var mapping in axisMappings)
            {
                var name = mapping.Name ?? string.Empty;
                axisNames.Add(name);
                axisDetails.Add(new
                {
                    name,
                    axis = mapping.Axis.ToString(),
                    gamepad = mapping.Gamepad.ToString(),
                    positiveButton = mapping.PositiveButton.ToString(),
                    negativeButton = mapping.NegativeButton.ToString(),
                    gamepadPositiveButton = mapping.GamepadPositiveButton.ToString(),
                    gamepadNegativeButton = mapping.GamepadNegativeButton.ToString(),
                    deadZone = mapping.DeadZone,
                    sensitivity = mapping.Sensitivity,
                    gravity = mapping.Gravity,
                    scale = mapping.Scale,
                    snap = mapping.Snap,
                });
            }

            var checks = new List<object>();
            var warnings = new List<string>();
            var failed = false;
            var keyboardAvailable = Input.Keyboard != null;
            var mouseAvailable = Input.Mouse != null;
            checks.Add(new
            {
                id = "device.keyboard",
                status = keyboardAvailable ? "pass" : "unavailable",
                message = keyboardAvailable ? "Runtime keyboard is available." : "Runtime keyboard is unavailable.",
            });
            checks.Add(new
            {
                id = "device.mouse",
                status = mouseAvailable ? "pass" : "unavailable",
                message = mouseAvailable ? "Runtime mouse is available." : "Runtime mouse is unavailable.",
            });

            if (actionMappings.Length == 0)
                warnings.Add("No action mappings are configured.");
            if (axisMappings.Length == 0)
                warnings.Add("No axis mappings are configured.");

            var keySamples = new List<object>();
            foreach (var name in requestedKeys)
            {
                if (!Enum.TryParse(name, true, out KeyboardKeys key) || key is KeyboardKeys.None or KeyboardKeys.MAX)
                {
                    failed = true;
                    keySamples.Add(new { name, status = "fail", error = $"Unknown keyboard key '{name}'." });
                    continue;
                }

                if (!keyboardAvailable)
                {
                    failed = true;
                    keySamples.Add(new { name, status = "fail", key = key.ToString(), error = "Runtime keyboard is unavailable." });
                    continue;
                }

                keySamples.Add(new
                {
                    name,
                    status = "pass",
                    key = key.ToString(),
                    pressed = Input.GetKey(key),
                    down = Input.GetKeyDown(key),
                    up = Input.GetKeyUp(key),
                });
            }

            var axisSamples = new List<object>();
            foreach (var name in requestedAxes)
            {
                var mapped = ContainsName(axisNames, name);
                if (!mapped)
                {
                    failed = true;
                    axisSamples.Add(new { name, status = "fail", mapped = false, value = 0.0f, raw = 0.0f, error = $"Axis mapping '{name}' was not found." });
                    continue;
                }

                axisSamples.Add(new
                {
                    name,
                    status = "pass",
                    mapped = true,
                    value = Input.GetAxis(name),
                    raw = Input.GetAxisRaw(name),
                });
            }

            var actionSamples = new List<object>();
            foreach (var name in requestedActions)
            {
                var mapped = ContainsName(actionNames, name);
                if (!mapped)
                {
                    failed = true;
                    actionSamples.Add(new { name, status = "fail", mapped = false, pressed = false, state = string.Empty, error = $"Action mapping '{name}' was not found." });
                    continue;
                }

                actionSamples.Add(new
                {
                    name,
                    status = "pass",
                    mapped = true,
                    pressed = Input.GetAction(name),
                    state = Input.GetActionState(name).ToString(),
                });
            }

            var activeKeys = new List<string>();
            if (keyboardAvailable)
            {
                foreach (var key in Enum.GetValues<KeyboardKeys>())
                {
                    if (key is KeyboardKeys.None or KeyboardKeys.MAX)
                        continue;
                    if (Input.GetKey(key))
                        activeKeys.Add(key.ToString());
                }
            }

            var activeButtons = new List<string>();
            object mouseState;
            if (mouseAvailable)
            {
                foreach (var button in Enum.GetValues<MouseButton>())
                {
                    if (button is MouseButton.None or MouseButton.MAX)
                        continue;
                    if (Input.GetMouseButton(button))
                        activeButtons.Add(button.ToString());
                }

                var position = Input.MousePosition;
                var delta = Input.MousePositionDelta;
                mouseState = new
                {
                    available = true,
                    position = new { x = position.X, y = position.Y },
                    delta = new { x = delta.X, y = delta.Y },
                    scrollDelta = Input.MouseScrollDelta,
                    activeButtons,
                };
            }
            else
            {
                mouseState = new { available = false, activeButtons };
            }

            object cursorState;
            try
            {
                cursorState = new
                {
                    available = true,
                    visible = Screen.CursorVisible,
                    lockMode = Screen.CursorLock.ToString(),
                };
            }
            catch (Exception exception)
            {
                cursorState = new { available = false, error = exception.Message };
            }

            return new
            {
                status = failed ? "fail" : "pass",
                checks,
                warnings,
                devices = new
                {
                    keyboard = new { available = keyboardAvailable, activeKeys },
                    mouse = mouseState,
                    cursor = cursorState,
                },
                mappings = new
                {
                    actions = actionDetails,
                    axes = axisDetails,
                },
                samples = new
                {
                    keys = keySamples,
                    axes = axisSamples,
                    actions = actionSamples,
                },
            };
        }

        private static string[] ReadValues(JObject arguments, string name)
        {
            var token = arguments[name];
            if (token == null || token.Type == JTokenType.Null)
                return Array.Empty<string>();

            var values = new List<string>();
            if (token is JArray array)
            {
                foreach (var item in array)
                {
                    var value = item.Value<string>();
                    if (!string.IsNullOrWhiteSpace(value))
                        values.Add(value);
                }
            }
            else
            {
                var value = token.Value<string>();
                if (!string.IsNullOrWhiteSpace(value))
                    values.Add(value);
            }
            return values.ToArray();
        }

        private static bool ContainsName(List<string> names, string name)
        {
            foreach (var candidate in names)
            {
                if (string.Equals(candidate, name, StringComparison.OrdinalIgnoreCase))
                    return true;
            }
            return false;
        }
    }
}
