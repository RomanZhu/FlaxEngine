// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.Content;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI
{
    /// <summary>
    /// Draws scalable semantic icons used by editor lists and trees.
    /// </summary>
    internal static class SemanticIcons
    {
        internal enum Glyph
        {
            Model,
            Collider,
            Material,
            Texture,
            Scene,
            Prefab,
            Script,
            Audio,
            Animation,
            Json,
            Particles,
            Shader,
            Other,
        }

        public static Glyph ForContent(ContentItemSearchFilter filter)
        {
            return filter switch
            {
                ContentItemSearchFilter.Model or ContentItemSearchFilter.SkinnedModel => Glyph.Model,
                ContentItemSearchFilter.Material => Glyph.Material,
                ContentItemSearchFilter.Texture => Glyph.Texture,
                ContentItemSearchFilter.Scene => Glyph.Scene,
                ContentItemSearchFilter.Prefab => Glyph.Prefab,
                ContentItemSearchFilter.Script => Glyph.Script,
                ContentItemSearchFilter.Audio => Glyph.Audio,
                ContentItemSearchFilter.Animation => Glyph.Animation,
                ContentItemSearchFilter.Json => Glyph.Json,
                ContentItemSearchFilter.Particles => Glyph.Particles,
                ContentItemSearchFilter.Shader => Glyph.Shader,
                _ => Glyph.Other,
            };
        }

        public static Color GetContentColor(ContentItemSearchFilter filter, Style style)
        {
            return filter switch
            {
                ContentItemSearchFilter.Model or ContentItemSearchFilter.SkinnedModel or ContentItemSearchFilter.Prefab => new Color(0.30f, 0.66f, 0.98f),
                ContentItemSearchFilter.Material => new Color(0.92f, 0.40f, 0.42f),
                ContentItemSearchFilter.Texture => new Color(0.38f, 0.80f, 0.60f),
                ContentItemSearchFilter.Scene => new Color(0.92f, 0.76f, 0.30f),
                ContentItemSearchFilter.Script or ContentItemSearchFilter.Shader or ContentItemSearchFilter.Json => new Color(0.48f, 0.70f, 0.98f),
                ContentItemSearchFilter.Audio => new Color(0.72f, 0.50f, 0.94f),
                ContentItemSearchFilter.Animation or ContentItemSearchFilter.Particles => new Color(0.98f, 0.62f, 0.30f),
                _ => style.ForegroundGrey,
            };
        }

        private static bool TryGetSprite(Glyph glyph, out SpriteHandle sprite)
        {
            var icons = Editor.Instance?.Icons;
            if (icons == null)
            {
                sprite = SpriteHandle.Invalid;
                return false;
            }

            sprite = glyph switch
            {
                Glyph.Model or Glyph.Collider or Glyph.Prefab => icons.VisjectBoxClosed32,
                Glyph.Material => icons.ColorWheel128,
                Glyph.Texture => icons.Image64,
                Glyph.Scene => icons.Scene128,
                Glyph.Script => icons.CSharpScript128,
                Glyph.Audio => icons.AudioSettings128,
                Glyph.Animation => icons.Play64,
                Glyph.Json => icons.Json128,
                Glyph.Particles => icons.Foliage96,
                Glyph.Shader => icons.Code64,
                _ => icons.Document128,
            };
            return sprite.IsValid;
        }

        public static void Draw(Glyph glyph, Rectangle bounds, Color color)
        {
            if (TryGetSprite(glyph, out var sprite))
            {
                // Use the editor's authored, high-resolution sprites rather than wireframe paths.
                Render2D.DrawSprite(sprite, bounds, color);
                return;
            }

            var size = Mathf.Min(bounds.Width, bounds.Height);
            if (size <= 0.0f)
                return;

            var stroke = Mathf.Clamp(size / 12.0f, 1.0f, 2.0f);
            switch (glyph)
            {
            case Glyph.Model:
            case Glyph.Prefab:
                DrawCube(bounds, color, stroke);
                if (glyph == Glyph.Prefab)
                    DrawLine(bounds, 16.0f, 16.0f, 21.0f, 21.0f, color, stroke);
                break;
            case Glyph.Collider:
                DrawFrame(bounds, 4.0f, 4.0f, 20.0f, 20.0f, color, stroke);
                DrawCircle(bounds, 12.0f, 12.0f, 4.5f, color, stroke);
                break;
            case Glyph.Material:
                DrawCircle(bounds, 12.0f, 12.0f, 7.0f, color, stroke);
                DrawLine(bounds, 5.0f, 17.0f, 9.0f, 13.0f, color, stroke);
                break;
            case Glyph.Texture:
                DrawFrame(bounds, 3.0f, 4.0f, 21.0f, 20.0f, color, stroke);
                DrawCircle(bounds, 8.0f, 9.0f, 1.8f, color, stroke);
                DrawLine(bounds, 5.0f, 18.0f, 10.0f, 13.0f, color, stroke);
                DrawLine(bounds, 10.0f, 13.0f, 14.0f, 16.0f, color, stroke);
                DrawLine(bounds, 14.0f, 16.0f, 17.0f, 11.0f, color, stroke);
                DrawLine(bounds, 17.0f, 11.0f, 20.0f, 18.0f, color, stroke);
                break;
            case Glyph.Scene:
                DrawFrame(bounds, 4.0f, 4.0f, 20.0f, 20.0f, color, stroke);
                DrawLine(bounds, 5.0f, 17.0f, 10.0f, 12.0f, color, stroke);
                DrawLine(bounds, 10.0f, 12.0f, 14.0f, 16.0f, color, stroke);
                DrawLine(bounds, 14.0f, 16.0f, 19.0f, 9.0f, color, stroke);
                break;
            case Glyph.Script:
            case Glyph.Json:
                DrawCode(bounds, color, stroke, glyph == Glyph.Json);
                break;
            case Glyph.Audio:
                DrawLine(bounds, 14.0f, 5.0f, 14.0f, 17.0f, color, stroke);
                DrawLine(bounds, 14.0f, 5.0f, 20.0f, 3.0f, color, stroke);
                DrawLine(bounds, 20.0f, 3.0f, 20.0f, 14.0f, color, stroke);
                DrawCircle(bounds, 11.0f, 18.0f, 2.5f, color, stroke);
                DrawCircle(bounds, 17.0f, 15.0f, 2.5f, color, stroke);
                break;
            case Glyph.Animation:
                DrawFrame(bounds, 4.0f, 4.0f, 20.0f, 20.0f, color, stroke);
                DrawLine(bounds, 10.0f, 8.0f, 17.0f, 12.0f, color, stroke);
                DrawLine(bounds, 17.0f, 12.0f, 10.0f, 16.0f, color, stroke);
                DrawLine(bounds, 10.0f, 16.0f, 10.0f, 8.0f, color, stroke);
                break;
            case Glyph.Particles:
                DrawSpark(bounds, 12.0f, 12.0f, 7.0f, color, stroke);
                DrawSpark(bounds, 18.0f, 7.0f, 3.5f, color, stroke);
                DrawSpark(bounds, 6.0f, 18.0f, 2.5f, color, stroke);
                break;
            case Glyph.Shader:
                DrawLine(bounds, 9.0f, 5.0f, 5.0f, 12.0f, color, stroke);
                DrawLine(bounds, 5.0f, 12.0f, 9.0f, 19.0f, color, stroke);
                DrawLine(bounds, 15.0f, 5.0f, 19.0f, 12.0f, color, stroke);
                DrawLine(bounds, 19.0f, 12.0f, 15.0f, 19.0f, color, stroke);
                DrawLine(bounds, 13.0f, 4.0f, 11.0f, 20.0f, color, stroke);
                break;
            default:
                DrawDocument(bounds, color, stroke);
                break;
            }
        }

        private static Float2 Point(Rectangle bounds, float x, float y)
        {
            return new Float2(bounds.X + bounds.Width * x / 24.0f, bounds.Y + bounds.Height * y / 24.0f);
        }

        private static void DrawLine(Rectangle bounds, float x1, float y1, float x2, float y2, Color color, float thickness)
        {
            Render2D.DrawLine(Point(bounds, x1, y1), Point(bounds, x2, y2), color, thickness);
        }

        private static void DrawFrame(Rectangle bounds, float left, float top, float right, float bottom, Color color, float thickness)
        {
            DrawLine(bounds, left, top, right, top, color, thickness);
            DrawLine(bounds, right, top, right, bottom, color, thickness);
            DrawLine(bounds, right, bottom, left, bottom, color, thickness);
            DrawLine(bounds, left, bottom, left, top, color, thickness);
        }

        private static void DrawCube(Rectangle bounds, Color color, float thickness)
        {
            DrawLine(bounds, 4.0f, 7.0f, 12.0f, 3.0f, color, thickness);
            DrawLine(bounds, 12.0f, 3.0f, 20.0f, 7.0f, color, thickness);
            DrawLine(bounds, 20.0f, 7.0f, 20.0f, 17.0f, color, thickness);
            DrawLine(bounds, 20.0f, 17.0f, 12.0f, 21.0f, color, thickness);
            DrawLine(bounds, 12.0f, 21.0f, 4.0f, 17.0f, color, thickness);
            DrawLine(bounds, 4.0f, 17.0f, 4.0f, 7.0f, color, thickness);
            DrawLine(bounds, 4.0f, 7.0f, 12.0f, 11.0f, color, thickness);
            DrawLine(bounds, 12.0f, 11.0f, 20.0f, 7.0f, color, thickness);
            DrawLine(bounds, 12.0f, 11.0f, 12.0f, 21.0f, color, thickness);
        }

        private static void DrawDocument(Rectangle bounds, Color color, float thickness)
        {
            DrawFrame(bounds, 5.0f, 3.0f, 18.0f, 21.0f, color, thickness);
            DrawLine(bounds, 13.0f, 3.0f, 18.0f, 8.0f, color, thickness);
            DrawLine(bounds, 13.0f, 3.0f, 13.0f, 8.0f, color, thickness);
            DrawLine(bounds, 13.0f, 8.0f, 18.0f, 8.0f, color, thickness);
            DrawLine(bounds, 8.0f, 13.0f, 15.0f, 13.0f, color, thickness);
            DrawLine(bounds, 8.0f, 17.0f, 15.0f, 17.0f, color, thickness);
        }

        private static void DrawCode(Rectangle bounds, Color color, float thickness, bool braces)
        {
            if (braces)
            {
                DrawLine(bounds, 9.0f, 5.0f, 6.0f, 8.0f, color, thickness);
                DrawLine(bounds, 6.0f, 8.0f, 6.0f, 16.0f, color, thickness);
                DrawLine(bounds, 6.0f, 16.0f, 9.0f, 19.0f, color, thickness);
                DrawLine(bounds, 15.0f, 5.0f, 18.0f, 8.0f, color, thickness);
                DrawLine(bounds, 18.0f, 8.0f, 18.0f, 16.0f, color, thickness);
                DrawLine(bounds, 18.0f, 16.0f, 15.0f, 19.0f, color, thickness);
            }
            else
            {
                DrawLine(bounds, 9.0f, 5.0f, 4.0f, 12.0f, color, thickness);
                DrawLine(bounds, 4.0f, 12.0f, 9.0f, 19.0f, color, thickness);
                DrawLine(bounds, 15.0f, 5.0f, 20.0f, 12.0f, color, thickness);
                DrawLine(bounds, 20.0f, 12.0f, 15.0f, 19.0f, color, thickness);
            }
        }

        private static void DrawCircle(Rectangle bounds, float centerX, float centerY, float radius, Color color, float thickness)
        {
            const int points = 12;
            var previous = Point(bounds, centerX + radius, centerY);
            for (int i = 1; i <= points; i++)
            {
                var angle = Mathf.TwoPi * i / points;
                var current = Point(bounds, centerX + Mathf.Cos(angle) * radius, centerY + Mathf.Sin(angle) * radius);
                Render2D.DrawLine(previous, current, color, thickness);
                previous = current;
            }
        }

        private static void DrawSpark(Rectangle bounds, float centerX, float centerY, float radius, Color color, float thickness)
        {
            DrawLine(bounds, centerX - radius, centerY, centerX + radius, centerY, color, thickness);
            DrawLine(bounds, centerX, centerY - radius, centerX, centerY + radius, color, thickness);
            DrawLine(bounds, centerX - radius * 0.65f, centerY - radius * 0.65f, centerX + radius * 0.65f, centerY + radius * 0.65f, color, thickness);
            DrawLine(bounds, centerX + radius * 0.65f, centerY - radius * 0.65f, centerX - radius * 0.65f, centerY + radius * 0.65f, color, thickness);
        }
    }
}
