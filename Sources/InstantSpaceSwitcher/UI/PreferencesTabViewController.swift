import AppKit

final class PreferencesTabViewController: NSTabViewController {
  override func viewDidLoad() {
    super.viewDidLoad()

    tabStyle = .toolbar

    let generalTab = NSTabViewItem(viewController: GeneralSettingsViewController())
    generalTab.label = "General"
    generalTab.image = NSImage(systemSymbolName: "gearshape", accessibilityDescription: "General")

    let shortcutsTab = NSTabViewItem(viewController: KeyboardShortcutsViewController())
    shortcutsTab.label = "Keyboard"
    shortcutsTab.image = NSImage(systemSymbolName: "keyboard", accessibilityDescription: "Keyboard")

    let appHotkeysTab = NSTabViewItem(viewController: AppHotkeysViewController())
    appHotkeysTab.label = "Apps"
    appHotkeysTab.image = NSImage(systemSymbolName: "app.badge", accessibilityDescription: "Apps")

    addTabViewItem(generalTab)
    addTabViewItem(shortcutsTab)
    addTabViewItem(appHotkeysTab)
  }
}
