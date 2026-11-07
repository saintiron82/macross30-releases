import unittest
from types import SimpleNamespace
from unittest.mock import Mock
import tkinter as tk

from app import has_keyboard_focus


class PopupFocusTests(unittest.TestCase):
    def widget(self, focused):
        bridge=Mock()
        bridge.call.return_value=focused
        widget=SimpleNamespace(tk=bridge,_w='.!frame.!canvas')
        return widget

    def test_internal_combobox_popup_does_not_require_python_widget_lookup(self):
        # ttk creates this path in Tcl; Python's nametowidget raises KeyError.
        widget=self.widget('.!frame.!combobox.popdown.f.l')
        self.assertFalse(has_keyboard_focus(widget))
        widget.tk.call.assert_called_once_with('focus','-displayof',widget._w)

    def test_button_focus_still_draws_focus_indicator(self):
        widget=self.widget('.!frame.!canvas')
        self.assertTrue(has_keyboard_focus(widget))

    def test_no_focus_does_not_draw_indicator(self):
        self.assertFalse(has_keyboard_focus(self.widget('')))

    def test_window_destroyed_during_focus_event_is_safe(self):
        widget=self.widget('')
        widget.tk.call.side_effect=tk.TclError('bad window path name')
        self.assertFalse(has_keyboard_focus(widget))


if __name__=='__main__':unittest.main()
