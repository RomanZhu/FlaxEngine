// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using FlaxEditor.History;
using NUnit.Framework;
using Assert = FlaxEngine.Assertions.Assert;

namespace FlaxEditor.Tests
{
    [TestFixture]
    public class TestUndo
    {
        [Serializable]
        public class UndoObject
        {
            public int FieldInteger = 10;
            public float FieldFloat = 0.1f;
            public UndoObject FieldObject;

            public int PropertyInteger { get; set; } = -10;
            public float PropertyFloat { get; set; } = -0.1f;
            public UndoObject PropertyObject { get; set; }

            public UndoObject()
            {
            }

            public UndoObject(bool addInstance)
            {
                if (!addInstance)
                {
                    return;
                }
                FieldObject = new UndoObject
                {
                    FieldInteger = 1,
                    FieldFloat = 1.1f,
                    FieldObject = new UndoObject(),
                    PropertyFloat = 1.1f,
                    PropertyInteger = 1,
                    PropertyObject = null
                };
                PropertyObject = new UndoObject
                {
                    FieldInteger = -1,
                    FieldFloat = -1.1f,
                    FieldObject = null,
                    PropertyFloat = -1.1f,
                    PropertyInteger = -1,
                    PropertyObject = new UndoObject()
                };
            }
        }

        private sealed class MetadataUndoAction : IUndoAction, IUndoActionMetadata
        {
            public string ActionString { get; set; }

            public UndoActionInfo ActionInfo { get; set; }

            public Action DoAction { get; set; }

            public Action UndoAction { get; set; }

            public void Do()
            {
                DoAction?.Invoke();
            }

            public void Undo()
            {
                UndoAction?.Invoke();
            }

            public void Dispose()
            {
            }
        }

        private sealed class MetadataOwner
        {
        }

        [Test]
        public void UndoTestBasic()
        {
            var undo = new Undo();

            var instance = new UndoObject(true);
            undo.RecordBegin(instance, "Basic");
            instance.FieldFloat = 0;
            instance.FieldInteger = 0;
            instance.FieldObject = null;
            instance.PropertyFloat = 0;
            instance.PropertyInteger = 0;
            instance.PropertyObject = null;
            undo.RecordEnd();
            BasicUndoRedo(undo, instance);

            instance = new UndoObject(true);
            undo.RecordAction(instance, "Basic", () =>
            {
                instance.FieldFloat = 0;
                instance.FieldInteger = 0;
                instance.FieldObject = null;
                instance.PropertyFloat = 0;
                instance.PropertyInteger = 0;
                instance.PropertyObject = null;
            });
            BasicUndoRedo(undo, instance);

            object generic = new UndoObject(true);
            undo.RecordAction(generic, "Basic", (i) =>
            {
                ((UndoObject)i).FieldFloat = 0;
                ((UndoObject)i).FieldInteger = 0;
                ((UndoObject)i).FieldObject = null;
                ((UndoObject)i).PropertyFloat = 0;
                ((UndoObject)i).PropertyInteger = 0;
                ((UndoObject)i).PropertyObject = null;
            });
            BasicUndoRedo(undo, (UndoObject)generic);

            instance = new UndoObject(true);
            undo.RecordAction(instance, "Basic", (i) =>
            {
                i.FieldFloat = 0;
                i.FieldInteger = 0;
                i.FieldObject = null;
                i.PropertyFloat = 0;
                i.PropertyInteger = 0;
                i.PropertyObject = null;
            });
            BasicUndoRedo(undo, instance);

            instance = new UndoObject(true);
            using (new UndoBlock(undo, instance, "Basic"))
            {
                instance.FieldFloat = 0;
                instance.FieldInteger = 0;
                instance.FieldObject = null;
                instance.PropertyFloat = 0;
                instance.PropertyInteger = 0;
                instance.PropertyObject = null;
            }
            BasicUndoRedo(undo, instance);
        }

        private static void BasicUndoRedo(Undo undo, UndoObject instance)
        {
            Assert.AreEqual("Basic", undo.FirstUndoName);
            undo.PerformUndo();
            Assert.AreEqual(10, instance.FieldInteger);
            Assert.AreEqual(0.1f, instance.FieldFloat);
            Assert.AreNotEqual(null, instance.FieldObject);
            Assert.AreEqual(-10, instance.PropertyInteger);
            Assert.AreEqual(-0.1f, instance.PropertyFloat);
            Assert.AreNotEqual(null, instance.PropertyObject);
            Assert.AreEqual("Basic", undo.FirstRedoName);
            undo.PerformRedo();
            Assert.AreEqual(0, instance.FieldInteger);
            Assert.AreEqual(0, instance.FieldFloat);
            Assert.AreEqual(null, instance.FieldObject);
            Assert.AreEqual(0, instance.PropertyInteger);
            Assert.AreEqual(0, instance.PropertyFloat);
            Assert.AreEqual(null, instance.PropertyObject);
        }

        [Test]
        public void UndoTestLinkedParent()
        {
            var parent = new Undo();
            var child = new Undo(parent, new object());
            var instance = new UndoObject();

            child.RecordAction(instance, "Child Edit", i => i.FieldInteger = 25);

            Assert.AreEqual(25, instance.FieldInteger);
            Assert.AreEqual(1, child.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(1, parent.UndoOperationsStack.HistoryCount);
            Assert.AreEqual("Child Edit", parent.FirstUndoName);

            parent.PerformUndo();

            Assert.AreEqual(10, instance.FieldInteger);
            Assert.AreEqual(0, child.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(1, child.UndoOperationsStack.ReverseCount);
            Assert.AreEqual("Child Edit", parent.FirstRedoName);

            parent.PerformRedo();

            Assert.AreEqual(25, instance.FieldInteger);
            Assert.AreEqual(1, child.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(0, child.UndoOperationsStack.ReverseCount);
        }

        [Test]
        public void UndoTestPerformingUndoRedoState()
        {
            var undo = new Undo();
            var wasUndoing = false;
            var wasRedoing = false;

            undo.AddAction(new MetadataUndoAction
            {
                ActionString = "State",
                UndoAction = () => wasUndoing = undo.IsPerformingUndoRedo,
                DoAction = () => wasRedoing = undo.IsPerformingUndoRedo,
            });

            Assert.AreEqual(false, undo.IsPerformingUndoRedo);
            undo.PerformUndo();
            Assert.AreEqual(true, wasUndoing);
            Assert.AreEqual(false, undo.IsPerformingUndoRedo);
            undo.PerformRedo();
            Assert.AreEqual(true, wasRedoing);
            Assert.AreEqual(false, undo.IsPerformingUndoRedo);

            var parent = new Undo();
            var child = new Undo(parent, new object());
            var childWasUndoing = false;
            var parentWasUndoing = false;

            child.AddAction(new MetadataUndoAction
            {
                ActionString = "Linked State",
                UndoAction = () =>
                {
                    childWasUndoing = child.IsPerformingUndoRedo;
                    parentWasUndoing = parent.IsPerformingUndoRedo;
                },
            });

            parent.PerformUndo();
            Assert.AreEqual(true, childWasUndoing);
            Assert.AreEqual(true, parentWasUndoing);
            Assert.AreEqual(false, child.IsPerformingUndoRedo);
            Assert.AreEqual(false, parent.IsPerformingUndoRedo);
        }

        [Test]
        public void UndoTestActionMetadata()
        {
            var parent = new Undo();
            var child = new Undo(parent, new MetadataOwner());
            var instance = new UndoObject();

            child.RecordAction(instance, "Child Edit", i => i.FieldInteger = 25);

            var linkedInfo = parent.FirstUndoInfo;
            Assert.AreEqual("Child Edit", linkedInfo.Operation);
            Assert.AreEqual(typeof(MetadataOwner).FullName, linkedInfo.DisplayEditorTypeName);

            var firstAction = new MetadataUndoAction
            {
                ActionString = "First",
                ActionInfo = new UndoActionInfo
                {
                    Operation = "First",
                    TargetType = UndoActionTargetType.Asset,
                    Flags = UndoActionFlags.RequiresReload,
                    SizeInBytes = 5,
                },
            };
            var secondAction = new MetadataUndoAction
            {
                ActionString = "Second",
                ActionInfo = new UndoActionInfo
                {
                    Operation = "Second",
                    TargetType = UndoActionTargetType.ContentItem,
                    Flags = UndoActionFlags.AffectsContentDatabase,
                    SizeInBytes = 7,
                },
            };
            var multiAction = new MultiUndoAction(new IUndoAction[] { firstAction, secondAction }, "Batch");

            var multiInfo = multiAction.ActionInfo;
            Assert.AreEqual("Batch", multiInfo.Operation);
            Assert.AreEqual(UndoActionTargetType.Multiple, multiInfo.TargetType);
            Assert.AreEqual(UndoActionFlags.RequiresReload | UndoActionFlags.AffectsContentDatabase, multiInfo.Flags);
            Assert.AreEqual(12, multiInfo.SizeInBytes);
        }

        [Test]
        public void UndoTestSizeCapacityUsesActionMetadata()
        {
            var undo = new Undo(10)
            {
                SizeCapacityInBytes = 10,
            };
            var discardedActionName = string.Empty;
            var discardReason = HistoryStackDiscardReason.Unknown;
            undo.ActionDiscarded += (action, reason) =>
            {
                discardedActionName = action.ActionString;
                discardReason = reason;
            };

            undo.AddAction(new MetadataUndoAction
            {
                ActionString = "First",
                ActionInfo = new UndoActionInfo
                {
                    Operation = "First",
                    SizeInBytes = 3,
                },
            });
            undo.AddAction(new MetadataUndoAction
            {
                ActionString = "Second",
                ActionInfo = new UndoActionInfo
                {
                    Operation = "Second",
                    SizeInBytes = 4,
                },
            });
            undo.AddAction(new MetadataUndoAction
            {
                ActionString = "Third",
                ActionInfo = new UndoActionInfo
                {
                    Operation = "Third",
                    SizeInBytes = 5,
                },
            });

            Assert.AreEqual(2, undo.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(9, undo.SizeInBytes);
            Assert.AreEqual("First", discardedActionName);
            Assert.AreEqual(HistoryStackDiscardReason.SizeLimit, discardReason);
            Assert.AreEqual("Third", undo.FirstUndoName);

            var undoInfos = undo.GetUndoActionInfos();
            Assert.AreEqual(2, undoInfos.Length);
            Assert.AreEqual("Third", undoInfos[0].Operation);
            Assert.AreEqual("Second", undoInfos[1].Operation);

            undo.PerformUndo();
            Assert.AreEqual(1, undo.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(1, undo.UndoOperationsStack.ReverseCount);
            Assert.AreEqual(9, undo.SizeInBytes);

            var redoInfos = undo.GetRedoActionInfos();
            Assert.AreEqual(1, redoInfos.Length);
            Assert.AreEqual("Third", redoInfos[0].Operation);
        }

        [Test]
        public void UndoTestLinkedChildClearRemovesParentAction()
        {
            var parent = new Undo();
            var child = new Undo(parent, new object());
            var instance = new UndoObject();

            child.RecordAction(instance, "Child Edit", i => i.FieldInteger = 25);
            child.Clear();

            Assert.AreEqual(0, child.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(0, child.UndoOperationsStack.ReverseCount);
            Assert.AreEqual(0, parent.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(0, parent.UndoOperationsStack.ReverseCount);
        }

        [Test]
        public void UndoTestLinkedParentHistoryPruningRemovesChildAction()
        {
            var parent = new Undo(1);
            var child = new Undo(parent, new object(), 10);
            var first = new UndoObject();
            var second = new UndoObject();

            child.RecordAction(first, "First Edit", i => i.FieldInteger = 25);
            child.RecordAction(second, "Second Edit", i => i.FieldInteger = 30);

            Assert.AreEqual(1, parent.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(1, child.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(25, first.FieldInteger);
            Assert.AreEqual(30, second.FieldInteger);

            parent.PerformUndo();

            Assert.AreEqual(25, first.FieldInteger);
            Assert.AreEqual(10, second.FieldInteger);
            Assert.AreEqual(0, child.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(1, child.UndoOperationsStack.ReverseCount);
        }

        [Test]
        public void UndoTestLinkedChildHistoryPruningRemovesParentAction()
        {
            var parent = new Undo(10);
            var child = new Undo(parent, new object(), 1);
            var first = new UndoObject();
            var second = new UndoObject();

            child.RecordAction(first, "First Edit", i => i.FieldInteger = 25);
            child.RecordAction(second, "Second Edit", i => i.FieldInteger = 30);

            Assert.AreEqual(1, parent.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(1, child.UndoOperationsStack.HistoryCount);
            Assert.AreEqual("Second Edit", parent.FirstUndoName);

            parent.PerformUndo();

            Assert.AreEqual(25, first.FieldInteger);
            Assert.AreEqual(10, second.FieldInteger);
            Assert.AreEqual(0, child.UndoOperationsStack.HistoryCount);
            Assert.AreEqual(1, child.UndoOperationsStack.ReverseCount);
        }

        [Test]
        public void UndoTestRecursive()
        {
            var undo = new Undo();
            var instance = new UndoObject(true);
            undo.RecordAction(instance, "Basic", (i) =>
            {
                i.FieldObject = new UndoObject();
                i.FieldObject.FieldObject = new UndoObject();
                i.FieldObject.FieldObject.FieldObject = new UndoObject();
                i.FieldObject.FieldObject.PropertyObject = new UndoObject();
                i.FieldObject.FieldObject.PropertyObject.FieldInteger = 99;
                i.PropertyObject = new UndoObject();
                i.PropertyObject.PropertyObject = new UndoObject();
                i.PropertyObject.PropertyObject.PropertyObject = new UndoObject();
                i.PropertyObject.PropertyObject.FieldObject = new UndoObject();
                i.PropertyObject.PropertyObject.FieldObject.FieldInteger = 99;
            });
            undo.PerformUndo();
            Assert.AreNotEqual(null, instance.FieldObject);
            Assert.AreNotEqual(null, instance.FieldObject.FieldObject);
            Assert.AreEqual(null, instance.FieldObject.FieldObject.FieldObject);
            Assert.AreEqual(null, instance.FieldObject.FieldObject.PropertyObject);
            Assert.AreNotEqual(null, instance.PropertyObject);
            Assert.AreNotEqual(null, instance.PropertyObject.PropertyObject);
            Assert.AreEqual(null, instance.PropertyObject.PropertyObject.PropertyObject);
            Assert.AreEqual(null, instance.PropertyObject.PropertyObject.FieldObject);
            undo.PerformRedo();
            Assert.AreNotEqual(null, instance.FieldObject);
            Assert.AreNotEqual(null, instance.FieldObject.FieldObject);
            Assert.AreNotEqual(null, instance.FieldObject.FieldObject.FieldObject);
            Assert.AreNotEqual(null, instance.FieldObject.FieldObject.PropertyObject);
            Assert.AreNotEqual(null, instance.PropertyObject);
            Assert.AreNotEqual(null, instance.PropertyObject.PropertyObject);
            Assert.AreNotEqual(null, instance.PropertyObject.PropertyObject.PropertyObject);
            Assert.AreNotEqual(null, instance.PropertyObject.PropertyObject.FieldObject);
            Assert.AreEqual(99, instance.FieldObject.FieldObject.PropertyObject.FieldInteger);
            Assert.AreEqual(99, instance.PropertyObject.PropertyObject.FieldObject.FieldInteger);
        }

        [Test]
        public void UndoTestRecursive2()
        {
            var undo = new Undo();
            var instance = new UndoObject(true)
            {
                FieldObject =
                {
                    FieldObject = new UndoObject(false),
                    PropertyObject = new UndoObject(false)
                },
                PropertyObject =
                {
                    FieldObject = new UndoObject(false),
                    PropertyObject = new UndoObject(false)
                }
            };
            using (new UndoBlock(undo, instance, "Edit"))
            {
                instance.FieldInteger = 1;
                instance.FieldObject.FieldInteger = 10;
                instance.PropertyObject.FieldInteger = 11;
                instance.FieldObject.FieldObject.FieldInteger = 22;
                instance.FieldObject.PropertyObject.FieldInteger = 23;
                instance.PropertyObject.FieldObject.FieldInteger = 24;
                instance.PropertyObject.PropertyObject.FieldInteger = 25;
            }
            undo.PerformUndo();
            Assert.AreEqual(10, instance.FieldInteger);
            Assert.AreEqual(1, instance.FieldObject.FieldInteger);
            Assert.AreEqual(-1, instance.PropertyObject.FieldInteger);
            Assert.AreEqual(10, instance.FieldObject.FieldObject.FieldInteger);
            Assert.AreEqual(10, instance.FieldObject.PropertyObject.FieldInteger);
            Assert.AreEqual(10, instance.PropertyObject.FieldObject.FieldInteger);
            Assert.AreEqual(10, instance.PropertyObject.PropertyObject.FieldInteger);
            undo.PerformRedo();
            Assert.AreEqual(1, instance.FieldInteger);
            Assert.AreEqual(10, instance.FieldObject.FieldInteger);
            Assert.AreEqual(11, instance.PropertyObject.FieldInteger);
            Assert.AreEqual(22, instance.FieldObject.FieldObject.FieldInteger);
            Assert.AreEqual(23, instance.FieldObject.PropertyObject.FieldInteger);
            Assert.AreEqual(24, instance.PropertyObject.FieldObject.FieldInteger);
            Assert.AreEqual(25, instance.PropertyObject.PropertyObject.FieldInteger);
        }

        [Serializable]
        public class UndoObjectWithArray : UndoObject
        {
            public UndoObjectWithArray[] FieldArray;
            public UndoObjectWithArray[] PropertyArray { get; set; }
        }

        [Test]
        public void UndoTestRecursiveArray()
        {
            var undo = new Undo();
            var instance = new UndoObjectWithArray
            {
                FieldArray = new[]
                {
                    new UndoObjectWithArray
                    {
                        FieldInteger = 33,
                        PropertyFloat = 3.3f,
                        FieldArray = new UndoObjectWithArray[1]
                        {
                            new UndoObjectWithArray
                            {
                                FieldFloat = 1000.0f
                            },
                        },
                        PropertyArray = new UndoObjectWithArray[1]
                        {
                            new UndoObjectWithArray
                            {
                                FieldFloat = 1000.0f
                            },
                        }
                    },
                    new UndoObjectWithArray
                    {
                        FieldInteger = 34,
                        PropertyFloat = 3.4f,
                        FieldArray = new UndoObjectWithArray[0]
                    },
                },
                PropertyArray = new[]
                {
                    new UndoObjectWithArray
                    {
                        FieldInteger = 33,
                        PropertyFloat = 3.3f,
                        FieldArray = new UndoObjectWithArray[1]
                        {
                            new UndoObjectWithArray
                            {
                                FieldFloat = 1000.0f
                            },
                        },
                        PropertyArray = new UndoObjectWithArray[1]
                        {
                            new UndoObjectWithArray
                            {
                                FieldFloat = 1000.0f
                            },
                        }
                    },
                    new UndoObjectWithArray
                    {
                        FieldInteger = 34,
                        PropertyFloat = 3.4f,
                        FieldArray = new UndoObjectWithArray[0]
                    },
                },
            };
            using (new UndoBlock(undo, instance, "Edit"))
            {
                instance.FieldArray[0].FieldInteger = 11;
                instance.FieldArray[0].PropertyFloat = 1.1f;
                instance.FieldArray[1].FieldInteger = 12;
                instance.FieldArray[1].PropertyFloat = 1.2f;
                instance.FieldArray[0].FieldArray = null;
                instance.FieldArray[0].PropertyArray[0].FieldFloat = -1;
                instance.PropertyArray[0].FieldInteger = -11;
                instance.PropertyArray[0].PropertyFloat = -1.1f;
                instance.PropertyArray[1].FieldInteger = -12;
                instance.PropertyArray[1].PropertyFloat = -1.2f;
                instance.PropertyArray[0].FieldArray = null;
                instance.PropertyArray[0].PropertyArray[0].FieldFloat = 1;
            }
            undo.PerformUndo();
            Assert.AreEqual(2, instance.FieldArray.Length);
            Assert.AreEqual(33, instance.FieldArray[0].FieldInteger);
            Assert.AreEqual(3.3f, instance.FieldArray[0].PropertyFloat);
            Assert.AreEqual(34, instance.FieldArray[1].FieldInteger);
            Assert.AreEqual(3.4f, instance.FieldArray[1].PropertyFloat);
            Assert.AreNotEqual(null, instance.FieldArray[0].FieldArray);
            Assert.AreEqual(1000.0f, instance.FieldArray[0].PropertyArray[0].FieldFloat);
            Assert.AreEqual(2, instance.PropertyArray.Length);
            Assert.AreEqual(33, instance.PropertyArray[0].FieldInteger);
            Assert.AreEqual(3.3f, instance.PropertyArray[0].PropertyFloat);
            Assert.AreEqual(34, instance.PropertyArray[1].FieldInteger);
            Assert.AreEqual(3.4f, instance.PropertyArray[1].PropertyFloat);
            Assert.AreNotEqual(null, instance.PropertyArray[0].FieldArray);
            Assert.AreEqual(1000.0f, instance.PropertyArray[0].PropertyArray[0].FieldFloat);
            undo.PerformRedo();
            Assert.AreEqual(11, instance.FieldArray[0].FieldInteger);
            Assert.AreEqual(1.1f, instance.FieldArray[0].PropertyFloat);
            Assert.AreEqual(12, instance.FieldArray[1].FieldInteger);
            Assert.AreEqual(1.2f, instance.FieldArray[1].PropertyFloat);
            Assert.AreEqual(null, instance.FieldArray[0].FieldArray);
            Assert.AreEqual(-1, instance.FieldArray[0].PropertyArray[0].FieldFloat);
            Assert.AreEqual(-11, instance.PropertyArray[0].FieldInteger);
            Assert.AreEqual(-1.1f, instance.PropertyArray[0].PropertyFloat);
            Assert.AreEqual(-12, instance.PropertyArray[1].FieldInteger);
            Assert.AreEqual(-1.2f, instance.PropertyArray[1].PropertyFloat);
            Assert.AreEqual(null, instance.PropertyArray[0].FieldArray);
            Assert.AreEqual(1, instance.PropertyArray[0].PropertyArray[0].FieldFloat);
        }

        [Serializable]
        class UndoObjectArray
        {
            public int[] MyInts;
            public int[] MyIntsProp { get; set; }
        }

        [Test]
        public void UndoTestArray()
        {
            var undo = new Undo();
            var instance = new UndoObjectArray
            {
                MyInts = new[]
                {
                    1,
                    2,
                    3,
                    4,
                    5
                },
                MyIntsProp = new[]
                {
                    1,
                    2,
                    3,
                    4,
                    5
                },
            };
            using (new UndoBlock(undo, instance, "Edit"))
            {
                instance.MyInts[0] = 11;
                instance.MyInts[1] = 12;
                instance.MyInts[2] = 13;
                instance.MyInts[3] = 14;
                instance.MyInts[4] = 15;
                instance.MyIntsProp[1] = 500;
            }
            undo.PerformUndo();
            Assert.AreEqual(5, instance.MyInts.Length);
            Assert.AreEqual(1, instance.MyInts[0]);
            Assert.AreEqual(2, instance.MyInts[1]);
            Assert.AreEqual(3, instance.MyInts[2]);
            Assert.AreEqual(4, instance.MyInts[3]);
            Assert.AreEqual(5, instance.MyInts[4]);
            Assert.AreEqual(5, instance.MyIntsProp.Length);
            Assert.AreEqual(1, instance.MyIntsProp[0]);
            Assert.AreEqual(2, instance.MyIntsProp[1]);
            Assert.AreEqual(3, instance.MyIntsProp[2]);
            Assert.AreEqual(4, instance.MyIntsProp[3]);
            Assert.AreEqual(5, instance.MyIntsProp[4]);
            undo.PerformRedo();
            Assert.AreEqual(5, instance.MyInts.Length);
            Assert.AreEqual(11, instance.MyInts[0]);
            Assert.AreEqual(12, instance.MyInts[1]);
            Assert.AreEqual(13, instance.MyInts[2]);
            Assert.AreEqual(14, instance.MyInts[3]);
            Assert.AreEqual(15, instance.MyInts[4]);
            Assert.AreEqual(5, instance.MyIntsProp.Length);
            Assert.AreEqual(1, instance.MyIntsProp[0]);
            Assert.AreEqual(500, instance.MyIntsProp[1]);
            Assert.AreEqual(3, instance.MyIntsProp[2]);
            Assert.AreEqual(4, instance.MyIntsProp[3]);
            Assert.AreEqual(5, instance.MyIntsProp[4]);
        }
    }
}
#endif
