// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Linq;
using FlaxEditor.History;
using NUnit.Framework;
using Assert = FlaxEngine.Assertions.Assert;

namespace FlaxEditor.Tests
{
    [TestFixture]
    public class TestHistoryStack
    {
        public class HistoryTestObject : IHistoryAction
        {
            public int Item;

            public HistoryTestObject(int item)
            {
                Item = item;
            }

            public override bool Equals(object obj)
            {
                var historyTestObject = (HistoryTestObject)obj;
                return historyTestObject != null && Item == historyTestObject.Item;
            }

            public static implicit operator int(HistoryTestObject obj)
            {
                return obj.Item;
            }

            public static implicit operator HistoryTestObject(int obj)
            {
                return new HistoryTestObject(obj);
            }

            public override string ToString()
            {
                return Item.ToString();
            }

            public override int GetHashCode()
            {
                var hashCode = -606588576;
                hashCode = hashCode * -1521134295 + Item.GetHashCode();
                return hashCode;
            }

            public string ActionString { get; set; }

            public void Dispose()
            {
            }
        }

        private sealed class NavigationTestAction : INavigationHistoryAction, INavigationHistoryDestination
        {
            public readonly int Destination;
            public int BackCount;
            public bool Disposed;

            public NavigationTestAction(int destination)
            {
                Destination = destination;
            }

            public string ActionString => "Navigation";

            public bool IsSameDestination(INavigationHistoryAction other)
            {
                return other is NavigationTestAction action && action.Destination == Destination;
            }

            public void NavigateBack()
            {
                BackCount++;
            }

            public void NavigateForward()
            {
            }

            public void Dispose()
            {
                Disposed = true;
            }
        }

        [Test]
        public void HistoryStackTestBasic()
        {
            var stack = new HistoryStack(50);
            for (int i = 0; i < 80; i++)
            {
                stack.Push(new HistoryTestObject(i));
            }
            for (int i = 80 - 1; i >= 80 - 50; i--)
            {
                Assert.AreEqual(i, (int)(HistoryTestObject)stack.PopHistory());
                Assert.AreEqual(i, (int)(HistoryTestObject)stack.PeekReverse());
            }
            for (int i = 80 - 50; i < 80; i++)
            {
                Assert.AreEqual(i, (int)(HistoryTestObject)stack.PopReverse());
                Assert.AreEqual(i, (int)(HistoryTestObject)stack.PeekHistory());
            }
        }

        [Test]
        public void HistoryStackTestEmptyHistory()
        {
            var stack = new HistoryStack(50);
            Assert.AreEqual(null, stack.PopHistory());
            Assert.AreEqual(null, stack.PopReverse());
        }

        [Test]
        public void HistoryStackTestTravel()
        {
            var stack = new HistoryStack(50);
            for (int i = 0; i < 80; i++)
            {
                stack.Push(new HistoryTestObject(i));
            }
            Assert.AreEqual(0, stack.ReverseCount);
            Assert.AreEqual(60, (int)(HistoryTestObject)stack.TravelBack(20));
            Assert.AreEqual(20, stack.ReverseCount);
            Assert.AreEqual(30, stack.HistoryCount);
            Assert.AreEqual(74, (int)(HistoryTestObject)stack.TravelReverse(15));
            Assert.AreEqual(5, stack.ReverseCount);
            Assert.AreEqual(45, stack.HistoryCount);
            Assert.AreEqual(79, (int)(HistoryTestObject)stack.TravelReverse(5));
            Assert.AreEqual(0, stack.ReverseCount);
            Assert.AreEqual(50, stack.HistoryCount);
            Assert.AreEqual(30, (int)(HistoryTestObject)stack.TravelBack(50));
            Assert.AreEqual(50, stack.ReverseCount);
            Assert.AreEqual(0, stack.HistoryCount);
        }

        [Test]
        public void HistoryStackTestExceptions()
        {
            var stack = new HistoryStack(50);
            for (int i = 0; i < 80; i++)
            {
                stack.Push(new HistoryTestObject(i));
            }
            Assert.ExceptionExpected(typeof(ArgumentOutOfRangeException), () => { stack.TravelBack(-5); });
            Assert.ExceptionExpected(typeof(ArgumentOutOfRangeException), () => { stack.TravelBack(0); });
            Assert.ExceptionExpected(typeof(ArgumentOutOfRangeException), () => { stack.TravelReverse(-5); });
            Assert.ExceptionExpected(typeof(ArgumentOutOfRangeException), () => { stack.TravelReverse(0); });
        }

        [Test]
        public void HistoryStackTestDropReverse()
        {
            var stack = new HistoryStack(50);
            for (int i = 0; i < 40; i++)
            {
                stack.Push(new HistoryTestObject(i));
            }
            stack.TravelBack(5);
            stack.Push(new HistoryTestObject(100));
            Assert.AreEqual(0, stack.ReverseCount);
            Assert.AreEqual(36, stack.HistoryCount);
            Assert.AreEqual(100, (int)(HistoryTestObject)stack.PeekHistory());
            Assert.AreEqual(null, stack.PeekReverse());
        }

        [Test]
        public void HistoryStackTestRemoveAll()
        {
            var stack = new HistoryStack(50);
            for (int i = 0; i < 10; i++)
            {
                stack.Push(new HistoryTestObject(i));
            }

            stack.TravelBack(3);
            stack.RemoveAll(x => ((HistoryTestObject)x).Item % 2 == 0);

            Assert.AreEqual(3, stack.HistoryCount);
            Assert.AreEqual(2, stack.ReverseCount);
            CollectionAssert.AreEqual(new[] { 5, 3, 1 }, stack.GetHistoryActions().Select(x => (int)(HistoryTestObject)x).ToArray());
            CollectionAssert.AreEqual(new[] { 7, 9 }, stack.GetReverseActions().Select(x => (int)(HistoryTestObject)x).ToArray());
            while (stack.HistoryCount > 0)
                Assert.AreEqual(1, ((int)(HistoryTestObject)stack.PopHistory()) % 2);
            while (stack.ReverseCount > 0)
                Assert.AreEqual(1, ((int)(HistoryTestObject)stack.PopReverse()) % 2);
        }

        [Test]
        public void HistoryStackTestActionSnapshotsUseTopFirstOrder()
        {
            var stack = new HistoryStack(10);
            stack.Push(new HistoryTestObject(1));
            stack.Push(new HistoryTestObject(2));
            stack.Push(new HistoryTestObject(3));

            var undoActions = stack.GetHistoryActions();
            Assert.AreEqual(3, undoActions.Length);
            Assert.AreEqual(3, (int)(HistoryTestObject)undoActions[0]);
            Assert.AreEqual(2, (int)(HistoryTestObject)undoActions[1]);
            Assert.AreEqual(1, (int)(HistoryTestObject)undoActions[2]);

            stack.PopHistory();
            var redoActions = stack.GetReverseActions();
            Assert.AreEqual(1, redoActions.Length);
            Assert.AreEqual(3, (int)(HistoryTestObject)redoActions[0]);
        }

        [Test]
        public void HistoryStackTestSizeLimitPrunesOldestActions()
        {
            var disposed = -1;
            var discardReason = HistoryStackDiscardReason.Unknown;
            var stack = new HistoryStack(10, x => ((HistoryTestObject)x).Item);
            stack.ActionDiscarded += (x, reason) =>
            {
                disposed = ((HistoryTestObject)x).Item;
                discardReason = reason;
            };
            stack.HistorySizeLimitInBytes = 10;

            stack.Push(new HistoryTestObject(3));
            stack.Push(new HistoryTestObject(4));
            stack.Push(new HistoryTestObject(5));

            Assert.AreEqual(2, stack.HistoryCount);
            Assert.AreEqual(0, stack.ReverseCount);
            Assert.AreEqual(9, stack.HistorySizeInBytes);
            Assert.AreEqual(3, disposed);
            Assert.AreEqual(HistoryStackDiscardReason.SizeLimit, discardReason);
            Assert.AreEqual(5, (int)(HistoryTestObject)stack.PopHistory());
            Assert.AreEqual(4, (int)(HistoryTestObject)stack.PopHistory());
        }

        [Test]
        public void HistoryStackTestSizeLimitKeepsOversizedCurrentAction()
        {
            var stack = new HistoryStack(10, x => ((HistoryTestObject)x).Item)
            {
                HistorySizeLimitInBytes = 5,
            };

            stack.Push(new HistoryTestObject(3));
            stack.Push(new HistoryTestObject(8));

            Assert.AreEqual(1, stack.HistoryCount);
            Assert.AreEqual(8, stack.HistorySizeInBytes);
            Assert.AreEqual(8, (int)(HistoryTestObject)stack.PeekHistory());
        }

        [Test]
        public void NavigationHistoryTestDeduplicatesDestinations()
        {
            var history = new NavigationHistory();
            var first = new NavigationTestAction(1);
            var duplicate = new NavigationTestAction(1);

            history.AddAction(first);
            history.AddAction(duplicate);

            Assert.IsTrue(duplicate.Disposed);
            Assert.IsTrue(history.CanGoBack);

            history.GoBack();

            Assert.AreEqual(1, first.BackCount);
            Assert.IsFalse(history.CanGoBack);
        }

        [Test]
        public void NavigationHistoryTestActionSnapshotsUseTopFirstOrder()
        {
            var history = new NavigationHistory();
            history.AddAction(new NavigationTestAction(1));
            history.AddAction(new NavigationTestAction(2));

            var backActions = history.GetBackActions();
            Assert.AreEqual(2, backActions.Length);
            Assert.AreEqual(2, ((NavigationTestAction)backActions[0]).Destination);
            Assert.AreEqual(1, ((NavigationTestAction)backActions[1]).Destination);

            history.GoBack();
            var forwardActions = history.GetForwardActions();
            Assert.AreEqual(1, forwardActions.Length);
            Assert.AreEqual(2, ((NavigationTestAction)forwardActions[0]).Destination);
        }
    }
}
#endif
