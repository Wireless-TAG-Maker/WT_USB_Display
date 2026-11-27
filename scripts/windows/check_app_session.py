"""Exercise installed-app connection decisions without touching USB or Windows displays."""
from contextlib import ExitStack, nullcontext, redirect_stdout
from io import StringIO
import json
from pathlib import Path
from queue import SimpleQueue
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host/windows"))
import display_paths as dp
import serve


class DisplaySessionTest(unittest.TestCase):
    def setUp(self):
        stack = ExitStack()
        self.addCleanup(stack.close)
        self.folder = Path(stack.enter_context(tempfile.TemporaryDirectory()))
        self.primary = dict(name="desktop", adapter="physical", primary=True, orientation=0,
                            x=0, y=0, width=1920, height=1080)
        self.extended = dict(name="panel", adapter=serve.ADAPTER, primary=False, orientation=0,
                             x=1920, y=0, width=1440, height=900)
        self.displays = [self.primary]
        self.devices = [object()]
        self.usb = MagicMock()
        self.usb.devices.side_effect = lambda: nullcontext(self.devices[:])
        self.usb.dll.libusb_get_device_speed.return_value = 3
        stack.enter_context(patch.multiple(serve, ROOT=self.folder, STOP=self.folder / "stop.request",
                                          WATCH=True, last_status=None))
        stack.enter_context(redirect_stdout(StringIO()))
        stack.enter_context(patch.object(serve.time, "sleep"))
        stack.enter_context(patch.object(serve, "devs", side_effect=lambda: self.displays[:]))
        self.mode = stack.enter_context(patch.object(serve, "set_test_mode"))
        self.control = stack.enter_context(patch.object(serve, "command", side_effect=self.monitor_command))
        self.worker = stack.enter_context(patch.object(serve, "run_worker"))
        self.worker.return_value = 0
        self.addition = stack.enter_context(patch.object(serve, "VirtualDisplayAddition")).return_value
        self.addition.already_present = False
        self.addition.target = None
        self.addition.try_extend.return_value = False
        self.addition.can_remove.return_value = True

    def monitor_command(self, action):
        if action == "add":
            self.displays.append(self.extended)
        if action == "remove":
            self.displays.remove(self.extended)

    def status(self):
        return json.loads((self.folder / "session.json").read_text())["status"]

    def test_unplug_and_reconnect_release_only_owned_display(self):
        self.devices.clear()
        serve.connect_if_ready(self.usb)
        self.assertEqual(self.status(), "unplugged")
        self.control.assert_not_called()
        def unplug(display):
            self.devices.clear()
            return 0

        self.worker.side_effect = unplug
        for _ in range(2):
            self.devices.append(object())
            serve.connect_if_ready(self.usb)
            self.assertEqual(self.displays, [self.primary])
            serve.connect_if_ready(self.usb)
            self.assertEqual(self.status(), "unplugged")
        self.assertEqual([c.args[0] for c in self.control.call_args_list], ["probe", "add", "remove"] * 2)
        self.assertEqual(self.worker.call_count, 2)
        self.assertEqual(self.mode.call_count, 2)
        self.addition.try_extend.assert_not_called()

    def test_connect_uses_the_only_compatible_display_in_one_attempt(self):
        self.displays.append(self.extended)
        selected = serve.connect_if_ready(self.usb, auto_select=True)
        self.worker.assert_called_once_with(self.extended)
        self.assertEqual(selected, serve.display_choice(self.extended))
        self.assertEqual(self.displays, [self.primary, self.extended])
        self.assertEqual([c.args for c in self.control.call_args_list], [("probe",)])
        self.mode.assert_not_called()

    def test_background_selection_is_checked_and_reused_after_replug(self):
        self.displays.append(self.extended)
        serve.connect_if_ready(self.usb)
        self.assertEqual(self.status(), "existing_available")
        serve.connect_if_ready(self.usb, choice="a different display")
        self.worker.assert_not_called()
        selected = serve.connect_if_ready(self.usb, choice=serve.display_choice(self.extended))
        self.worker.assert_called_once_with(self.extended)
        self.devices.clear()
        serve.connect_if_ready(self.usb, choice=selected)
        self.assertEqual(self.status(), "unplugged")
        self.devices.append(object())
        serve.connect_if_ready(self.usb, choice=selected)
        self.assertEqual(self.worker.call_count, 2)
        self.assertEqual(self.displays, [self.primary, self.extended])
        self.assertTrue(all(c.args == ("probe",) for c in self.control.call_args_list))
        self.mode.assert_not_called()

    def test_connect_does_not_guess_between_virtual_outputs_or_capture_an_unsuitable_one(self):
        other = dict(self.extended, name="other-panel", x=3360)
        for candidates in ([self.extended, other], [dict(self.extended, primary=True)],
                           [dict(self.extended, x=0)], [dict(self.extended, orientation=1)],
                           [dict(self.extended, width=1280, height=720)]):
            with self.subTest(candidates=candidates):
                self.displays[:] = [self.primary, *candidates]
                serve.connect_if_ready(self.usb, auto_select=True)
                self.worker.assert_not_called()
                self.mode.assert_not_called()
                self.assertTrue(all(c.args == ("probe",) for c in self.control.call_args_list))

    def test_watch_waits_for_usb_then_reconnects_only_the_selected_source(self):
        self.displays.append(self.extended)
        replacement = dict(self.extended, name="replacement-panel")
        cycles = [(False, self.extended), (True, self.extended), (False, self.extended),
                  (True, self.extended), (True, replacement)]
        calls = []
        connect = serve.connect_if_ready

        def next_cycle(usb, choice, auto_select):
            plugged, display = cycles[len(calls)]
            self.displays[1] = display
            self.devices[:] = [object()] if plugged else []
            result = connect(usb, choice, auto_select)
            calls.append(self.worker.call_count)
            if len(calls) == len(cycles):
                self.assertEqual(self.status(), "existing_available")
                serve.STOP.touch()
            return result

        with patch.object(serve, "connect_if_ready", side_effect=next_cycle), \
                patch.object(serve, "USB", return_value=self.usb), patch.object(serve.threading, "Thread"), \
                patch.object(serve, "display_choices", SimpleQueue()), patch.dict(serve.os.environ):
            serve.watch(auto_select=True)
        self.assertEqual(calls, [0, 1, 1, 2, 2])
        self.assertTrue(all(c.args == (self.extended,) for c in self.worker.call_args_list))

    def test_independent_cli_still_requires_its_own_display(self):
        self.displays.append(self.extended)
        with patch.object(serve, "WATCH", False), self.assertRaises(RuntimeError):
            serve.serve()
        self.worker.assert_not_called()
        self.control.assert_not_called()

    def test_setup_failure_releases_new_display(self):
        self.mode.side_effect = RuntimeError("mode unavailable")
        serve.connect_if_ready(self.usb)
        self.assertEqual(self.status(), "display_error")
        self.assertEqual(self.displays, [self.primary])
        self.worker.assert_not_called()

    def test_first_connection_extends_a_new_cloned_laptop_screen(self):
        self.primary.update(width=3120, height=2080)
        self.extended.update(x=3120)
        # 1|2 in Windows Settings still looks like one desktop to EnumDisplayDevices.
        def control(action):
            if action == "remove":
                self.displays[:] = [self.primary]

        def extend():
            self.displays.append(self.extended)
            return True

        self.control.side_effect = control
        self.addition.try_extend.side_effect = extend
        self.worker.side_effect = lambda display: self.devices.clear() or 0
        with patch.object(serve.time, "monotonic", side_effect=range(100)):
            serve.connect_if_ready(self.usb, auto_select=True)
        self.worker.assert_called_once_with(self.extended)
        self.mode.assert_called_once_with(self.extended["name"])
        self.addition.try_extend.assert_called_once()
        self.assertEqual([c.args[0] for c in self.control.call_args_list], ["probe", "add", "remove"])
        self.assertEqual(self.displays, [self.primary])

    def test_existing_cloned_output_is_not_replaced_or_removed(self):
        self.addition.already_present = True
        self.worker.side_effect = lambda display: self.devices.clear() or 0
        serve.connect_if_ready(self.usb, auto_select=True)
        self.assertEqual(self.status(), "extend_display")
        self.assertEqual([c.args[0] for c in self.control.call_args_list], ["probe"])
        self.worker.assert_not_called()

    def test_user_can_extend_after_windows_declines_the_change(self):
        self.addition.target = (0, 7, 1)
        self.addition.try_extend.side_effect = OSError("configuration rejected")
        def control(action):
            if action == "remove":
                self.displays[:] = [self.primary]

        state = serve.state
        def show(status, **extra):
            state(status, **extra)
            if status == "extend_display":
                self.assertIn("rejected", extra["reason"])
                self.displays.append(self.extended)  # User clicks Extend on the still-plugged target.

        self.control.side_effect = control
        self.worker.side_effect = lambda display: self.devices.clear() or 0
        with patch.object(serve.time, "monotonic", side_effect=range(100)), patch.object(serve, "state", side_effect=show):
            serve.connect_if_ready(self.usb, auto_select=True)
        self.assertEqual([c.args[0] for c in self.control.call_args_list], ["probe", "add", "remove"])
        self.worker.assert_called_once_with(self.extended)


def route(source_adapter, source_id, target_adapter, target_id, mode_index=None):
    p = dp.DisplayPath()
    p.source.adapter.low, p.source.id = source_adapter, source_id
    p.target.adapter.low, p.target.id, p.target.available = target_adapter, target_id, True
    p.flags = dp.PATH_SUPPORT_VIRTUAL_MODE
    p.source.mode_index = p.target.mode_index = dp.INVALID_INDEX
    if mode_index is not None:
        p.flags |= dp.PATH_ACTIVE
        p.source.mode_index = (mode_index << 16) | 0xffff
    return p


class DisplayPathTest(unittest.TestCase):
    def setUp(self):
        stack = ExitStack()
        self.addCleanup(stack.close)
        self.main = route(1, 0, 1, 8, 0)
        self.disabled = route(1, 1, 1, 9)
        self.main_mode = dp.DisplayMode(kind=1, id=0, adapter=self.main.source.adapter)
        self.main_mode.data.source = dp.SourceMode(3120, 2080, 4, dp.W.POINT(0, 0))
        self.before = dp.DisplayConfig([self.main, self.disabled], [self.main_mode])
        # Same desktop as the laptop, plus an inactive route through the actual Amyuni adapter.
        self.clone = route(1, 0, 7, 12, 0)
        self.own = route(7, 0, 7, 12)
        self.read = stack.enter_context(patch.object(dp.DisplayConfig, "read", return_value=self.before))
        stack.enter_context(patch.object(dp, "adapters", return_value=[
            SimpleNamespace(name="virtual", id="usbmmidd", label="Virtual display")]))
        def info(record, kind, endpoint):
            if kind == 1:
                record.name = "virtual" if endpoint.adapter.low == 7 else "physical"
            else:
                self.assertEqual((kind, dp.key(endpoint)), (3, dp.key(self.own.target)))
                record.width, record.height = 1920, 1080
            return record
        stack.enter_context(patch.object(dp, "device_info", side_effect=info))
        self.apply = stack.enter_context(patch.object(dp, "set_paths"))
        self.addition = dp.VirtualDisplayAddition([dict(x=0, y=0, width=3120)])
        self.read.return_value = dp.DisplayConfig([*self.before.paths, self.clone, self.own], [self.main_mode])

    def test_clone_gets_an_independent_source_and_keeps_original_modes(self):
        main = bytes(self.main)
        self.assertTrue(self.addition.try_extend())
        self.assertEqual([call.args[2] for call in self.apply.call_args_list], [dp.SDC_VALIDATE, dp.SDC_APPLY])
        paths, modes, _ = self.apply.call_args.args
        self.assertEqual(len(paths), 2)  # The connected but disabled physical target stays off.
        self.assertEqual(bytes(paths[0]), main)
        self.assertEqual((dp.key(paths[1].source), dp.key(paths[1].target)),
                         (dp.key(self.own.source), dp.key(self.own.target)))
        self.assertEqual(paths[1].source.mode_index, (1 << 16) | 0xffff)
        self.assertEqual(bytes(modes[0]), bytes(self.main_mode))
        source = modes[1].data.source
        self.assertEqual((source.position.x, source.position.y, source.width, source.height), (3120, 0, 1920, 1080))

    def test_rejected_configuration_is_never_applied(self):
        self.apply.side_effect = OSError("rejected")
        with self.assertRaises(OSError):
            self.addition.try_extend()
        self.assertEqual([call.args[2] for call in self.apply.call_args_list], [dp.SDC_VALIDATE])

    def test_other_hotplug_or_unidentified_clone_is_not_changed(self):
        for new in ([self.clone, self.own, route(1, 2, 1, 10)], [self.clone]):
            with self.subTest(count=len(new)):
                self.read.return_value = dp.DisplayConfig([*self.before.paths, *new], [self.main_mode])
                with self.assertRaises(RuntimeError):
                    self.addition.try_extend()
                self.apply.assert_not_called()

    def test_separate_virtual_screen_keeps_its_windows_mode(self):
        screen = route(7, 0, 7, 12, 1)
        mode = dp.DisplayMode(kind=1, id=0, adapter=screen.source.adapter)
        mode.data.source = dp.SourceMode(1440, 900, 4, dp.W.POINT(-1440, 100))
        self.read.return_value = dp.DisplayConfig([*self.before.paths, screen], [self.main_mode, mode])
        self.assertTrue(self.addition.try_extend())
        self.apply.assert_not_called()

    def test_concurrent_rearrangement_on_another_screen_is_preserved(self):
        other = route(1, 1, 1, 9, 1)
        original_mode = dp.DisplayMode(kind=1, id=1, adapter=other.source.adapter)
        original_mode.data.source = dp.SourceMode(1920, 1080, 4, dp.W.POINT(-1920, 100))
        moved = dp.DisplayMode.from_buffer_copy(original_mode)
        moved.data.source.position.y += 300
        self.addition.before = dp.DisplayConfig([self.main, other], [self.main_mode, original_mode])
        self.read.return_value = dp.DisplayConfig([self.main, other, self.clone, self.own], [self.main_mode, moved])
        with self.assertRaisesRegex(RuntimeError, "reconfigured"):
            self.addition.try_extend()
        self.apply.assert_not_called()


if __name__ == "__main__":
    unittest.main()
