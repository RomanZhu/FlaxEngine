// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using Flax.Build;
using Flax.Build.NativeCpp;

/// <summary>
/// Physics backend implementation selected at compile time.
/// </summary>
public enum PhysicsBackendType
{
    Empty,
    PhysX,
    Box3D,
}

/// <summary>
/// Physics module.
/// </summary>
public class Physics : EngineModule
{
    /// <summary>
    /// Enables using collisions cooking.
    /// </summary>
    public static bool WithCooking = true;

    /// <summary>
    /// Enables using vehicles simulation.
    /// </summary>
    public static bool WithVehicle = true;

    /// <summary>
    /// Enables using cloth simulation.
    /// </summary>
    public static bool WithCloth = true;

    /// <summary>
    /// Enables using PhysX library. Can be overriden by SetupPhysicsBackend.
    /// </summary>
    public static bool WithPhysX = false;

    /// <summary>
    /// The physics backend to compile into the engine.
    /// </summary>
    public static PhysicsBackendType Backend = PhysicsBackendType.Box3D;

    /// <summary>
    /// Physics system extension event to override for custom physics backend plugin.
    /// </summary>
    public static Action<Physics, BuildOptions> SetupPhysicsBackend = SetupSelectedPhysicsBackend;

    /// <inheritdoc />
    public override void Setup(BuildOptions options)
    {
        base.Setup(options);

        SetupPhysicsBackend(this, options);

        if (WithCooking)
        {
            options.PublicDefinitions.Add("COMPILE_WITH_PHYSICS_COOKING");
        }
    }

    private static void SetupSelectedPhysicsBackend(Physics physics, BuildOptions options)
    {
        switch (Backend)
        {
        case PhysicsBackendType.Empty:
            SetupPhysicsSources(physics, options);
            options.PrivateDefinitions.Add("COMPILE_WITH_EMPTY_PHYSICS");
            break;
        case PhysicsBackendType.PhysX:
            SetupPhysicsSources(physics, options, "PhysX");
            SetupPhysicsBackendPhysX(physics, options);
            break;
        case PhysicsBackendType.Box3D:
            SetupPhysicsSources(physics, options, "Box3D");
            SetupPhysicsBackendBox3D(physics, options);
            break;
        default:
            throw new Exception("Unknown physics backend " + Backend);
        }
    }

    private static void SetupPhysicsSources(Physics physics, BuildOptions options, params string[] backendFolders)
    {
        options.SourcePaths.Clear();
        options.SourceFiles.Clear();
        AddFiles(options, physics.FolderPath, false);
        AddFiles(options, Path.Combine(physics.FolderPath, "Actors"), true);
        AddFiles(options, Path.Combine(physics.FolderPath, "Colliders"), true);
        AddFiles(options, Path.Combine(physics.FolderPath, "Joints"), true);
        foreach (var folder in backendFolders)
            AddFiles(options, Path.Combine(physics.FolderPath, folder), true);
    }

    private static void AddFiles(BuildOptions options, string folder, bool recursive)
    {
        if (Directory.Exists(folder))
            options.SourceFiles.AddRange(Directory.GetFiles(folder, "*", recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly));
    }

    private static void SetupPhysicsBackendPhysX(Physics physics, BuildOptions options)
    {
        WithPhysX = true;
        options.PrivateDependencies.Add("PhysX");
        if (WithCloth)
            options.PrivateDependencies.Add("NvCloth");
    }

    private static void SetupPhysicsBackendBox3D(Physics physics, BuildOptions options)
    {
        WithPhysX = false;
        WithVehicle = false;
        WithCloth = false;
        options.PrivateDefinitions.Add("COMPILE_WITH_BOX3D");
        options.PrivateDependencies.Add("Box3D");
    }
}
