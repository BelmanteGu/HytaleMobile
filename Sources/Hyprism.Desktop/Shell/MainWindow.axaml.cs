// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;

namespace Hyprism.Desktop.Shell;

public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        PropertyChanged += OnWindowPropertyChanged;
        UpdateWindowStateIcon();
    }

    private void OnWindowPropertyChanged(object? sender, AvaloniaPropertyChangedEventArgs e)
    {
        if (e.Property == WindowStateProperty)
            UpdateWindowStateIcon();
    }

    private void UpdateWindowStateIcon()
    {
        var isRestored = WindowState is WindowState.Maximized or WindowState.FullScreen;
        MaximizeWindowIcon.IsVisible = !isRestored;
        RestoreWindowIcon.IsVisible = isRestored;
    }

    private void OnMinimizeClicked(object? sender, RoutedEventArgs e)
        => WindowState = WindowState.Minimized;

    private void OnMaximizeClicked(object? sender, RoutedEventArgs e)
        => ToggleMaximized();

    private void OnCloseClicked(object? sender, RoutedEventArgs e)
        => Close();

    private void ToggleMaximized()
        => WindowState = WindowState == WindowState.Maximized
            ? WindowState.Normal
            : WindowState.Maximized;
}
