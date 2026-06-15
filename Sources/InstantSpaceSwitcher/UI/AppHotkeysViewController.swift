import AppKit
import Combine

final class AppHotkeysViewController: NSViewController {
  private let store = AppSpaceHotkeyStore.shared
  private var cancellables = Set<AnyCancellable>()

  private let tableView = NSTableView()
  private let scrollView = NSScrollView()
  private let addButton = NSButton(title: "Add Fullscreen App…", target: nil, action: nil)
  private let statusLabel = NSTextField(labelWithString: "")

  private var mappings: [AppSpaceHotkeyMapping] = []

  override func loadView() {
    view = NSView(frame: NSRect(x: 0, y: 0, width: 620, height: 360))
  }

  override func viewDidLoad() {
    super.viewDidLoad()
    setupUI()
    bindStore()
    loadMappings()
  }

  private func setupUI() {
    let enabledColumn = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("enabled"))
    enabledColumn.title = "Enabled"
    enabledColumn.width = 70
    tableView.addTableColumn(enabledColumn)

    let appColumn = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("app"))
    appColumn.title = "Fullscreen App"
    appColumn.width = 230
    tableView.addTableColumn(appColumn)

    let shortcutColumn = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("shortcut"))
    shortcutColumn.title = "Shortcut"
    shortcutColumn.width = 190
    tableView.addTableColumn(shortcutColumn)

    let actionsColumn = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("actions"))
    actionsColumn.title = ""
    actionsColumn.width = 90
    tableView.addTableColumn(actionsColumn)

    tableView.delegate = self
    tableView.dataSource = self
    tableView.rowHeight = 30
    tableView.usesAlternatingRowBackgroundColors = true
    tableView.columnAutoresizingStyle = .uniformColumnAutoresizingStyle

    scrollView.documentView = tableView
    scrollView.hasVerticalScroller = true
    scrollView.hasHorizontalScroller = false
    scrollView.borderType = .bezelBorder
    scrollView.translatesAutoresizingMaskIntoConstraints = false

    addButton.target = self
    addButton.action = #selector(addFullscreenApp)
    addButton.translatesAutoresizingMaskIntoConstraints = false

    statusLabel.font = .systemFont(ofSize: 12)
    statusLabel.textColor = .secondaryLabelColor
    statusLabel.lineBreakMode = .byTruncatingTail
    statusLabel.translatesAutoresizingMaskIntoConstraints = false

    view.addSubview(scrollView)
    view.addSubview(addButton)
    view.addSubview(statusLabel)

    NSLayoutConstraint.activate([
      scrollView.topAnchor.constraint(equalTo: view.topAnchor, constant: 20),
      scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 20),
      scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -20),
      scrollView.bottomAnchor.constraint(equalTo: addButton.topAnchor, constant: -12),

      addButton.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 20),
      addButton.bottomAnchor.constraint(equalTo: view.bottomAnchor, constant: -20),

      statusLabel.centerYAnchor.constraint(equalTo: addButton.centerYAnchor),
      statusLabel.leadingAnchor.constraint(equalTo: addButton.trailingAnchor, constant: 16),
      statusLabel.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -20),
    ])
  }

  private func bindStore() {
    store.$mappings.receive(on: RunLoop.main).sink { [weak self] _ in
      self?.loadMappings()
    }.store(in: &cancellables)
  }

  private func loadMappings() {
    mappings = store.mappings
    tableView.reloadData()
  }

  @objc private func addFullscreenApp() {
    let detected = AppSpaceResolver.detectedSpaces()
    let fullscreen = detected.filter(\.isFullscreen)
    let candidates = fullscreen.isEmpty ? detected : fullscreen
    let existingKeys = Set(store.mappings.map { $0.bundleIdentifier ?? $0.ownerName })
    let available = candidates.filter { space in
      !existingKeys.contains(space.bundleIdentifier ?? space.ownerName)
    }

    guard !available.isEmpty else {
      NSSound.beep()
      setStatus("No unmapped fullscreen apps detected on the cursor display.", color: .systemRed)
      return
    }

    let popup = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 320, height: 28), pullsDown: false)
    for space in available {
      let suffix = space.isFullscreen ? "Space \(space.index + 1)" : "Space \(space.index + 1), app-owned"
      popup.addItem(withTitle: "\(space.displayName) (\(suffix))")
    }

    let alert = NSAlert()
    alert.messageText = "Add Fullscreen App Hotkey"
    alert.informativeText = "Choose a detected app Space, then record a shortcut in the table."
    alert.accessoryView = popup
    alert.addButton(withTitle: "Add")
    alert.addButton(withTitle: "Cancel")

    guard alert.runModal() == .alertFirstButtonReturn else { return }
    let selected = max(0, popup.indexOfSelectedItem)
    guard selected < available.count else { return }
    store.addMapping(for: available[selected])
    setStatus("Added \(available[selected].displayName). Record a shortcut to enable it.", color: .labelColor)
  }

  private func setStatus(_ message: String, color: NSColor) {
    statusLabel.stringValue = message
    statusLabel.textColor = color
  }
}

extension AppHotkeysViewController: NSTableViewDataSource {
  func numberOfRows(in tableView: NSTableView) -> Int {
    mappings.count
  }
}

extension AppHotkeysViewController: NSTableViewDelegate {
  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    let mapping = mappings[row]
    let column = tableColumn?.identifier.rawValue

    if column == "enabled" {
      let cellView = NSTableCellView()
      let checkbox = NSButton(checkboxWithTitle: "", target: self, action: #selector(toggleEnabled(_:)))
      checkbox.state = mapping.isEnabled ? .on : .off
      checkbox.tag = row
      checkbox.translatesAutoresizingMaskIntoConstraints = false
      cellView.addSubview(checkbox)
      NSLayoutConstraint.activate([
        checkbox.centerXAnchor.constraint(equalTo: cellView.centerXAnchor),
        checkbox.centerYAnchor.constraint(equalTo: cellView.centerYAnchor),
      ])
      return cellView
    }

    if column == "app" {
      let cellView = NSTableCellView()
      let textField = NSTextField(labelWithString: mapping.displayName)
      textField.lineBreakMode = .byTruncatingTail
      textField.textColor = mapping.isEnabled ? .labelColor : .disabledControlTextColor
      textField.translatesAutoresizingMaskIntoConstraints = false
      cellView.addSubview(textField)
      NSLayoutConstraint.activate([
        textField.leadingAnchor.constraint(equalTo: cellView.leadingAnchor, constant: 4),
        textField.trailingAnchor.constraint(equalTo: cellView.trailingAnchor, constant: -4),
        textField.centerYAnchor.constraint(equalTo: cellView.centerYAnchor),
      ])
      return cellView
    }

    if column == "shortcut" {
      let cellView = NSView()
      let recorder = ShortcutRecorderControl(frame: NSRect(x: 0, y: 0, width: 150, height: 22))
      recorder.currentShortcut = mapping.hotkey
      recorder.isEnabled = mapping.isEnabled
      recorder.translatesAutoresizingMaskIntoConstraints = false
      recorder.onRecordingComplete = { [weak self] combination in
        self?.handleRecordingResult(combination, for: mapping)
      }
      cellView.addSubview(recorder)
      NSLayoutConstraint.activate([
        recorder.leadingAnchor.constraint(equalTo: cellView.leadingAnchor, constant: 4),
        recorder.centerYAnchor.constraint(equalTo: cellView.centerYAnchor),
        recorder.widthAnchor.constraint(equalToConstant: 150),
        recorder.heightAnchor.constraint(equalToConstant: 22),
      ])
      return cellView
    }

    if column == "actions" {
      let cellView = NSView()
      let clearButton = NSButton(
        image: NSImage(systemSymbolName: "xmark.circle", accessibilityDescription: "Clear Shortcut")!,
        target: self,
        action: #selector(clearShortcut(_:)))
      clearButton.bezelStyle = .rounded
      clearButton.isBordered = false
      clearButton.tag = row
      clearButton.isEnabled = mapping.hotkey != nil
      clearButton.translatesAutoresizingMaskIntoConstraints = false

      let removeButton = NSButton(
        image: NSImage(systemSymbolName: "trash", accessibilityDescription: "Remove")!,
        target: self,
        action: #selector(removeMapping(_:)))
      removeButton.bezelStyle = .rounded
      removeButton.isBordered = false
      removeButton.tag = row
      removeButton.translatesAutoresizingMaskIntoConstraints = false

      cellView.addSubview(clearButton)
      cellView.addSubview(removeButton)
      NSLayoutConstraint.activate([
        clearButton.leadingAnchor.constraint(equalTo: cellView.leadingAnchor, constant: 4),
        clearButton.centerYAnchor.constraint(equalTo: cellView.centerYAnchor),
        clearButton.widthAnchor.constraint(equalToConstant: 24),
        clearButton.heightAnchor.constraint(equalToConstant: 24),

        removeButton.leadingAnchor.constraint(equalTo: clearButton.trailingAnchor, constant: 8),
        removeButton.centerYAnchor.constraint(equalTo: cellView.centerYAnchor),
        removeButton.widthAnchor.constraint(equalToConstant: 24),
        removeButton.heightAnchor.constraint(equalToConstant: 24),
      ])
      return cellView
    }

    return nil
  }

  @objc private func toggleEnabled(_ sender: NSButton) {
    guard sender.tag < mappings.count else { return }
    let mapping = mappings[sender.tag]
    store.setEnabled(sender.state == .on, for: mapping.id)
  }

  @objc private func clearShortcut(_ sender: NSButton) {
    guard sender.tag < mappings.count else { return }
    let mapping = mappings[sender.tag]
    store.updateHotkey(nil, for: mapping.id)
    setStatus("Cleared \(mapping.displayName) shortcut.", color: .labelColor)
  }

  @objc private func removeMapping(_ sender: NSButton) {
    guard sender.tag < mappings.count else { return }
    let mapping = mappings[sender.tag]
    store.removeMapping(id: mapping.id)
    setStatus("Removed \(mapping.displayName).", color: .labelColor)
  }

  private func handleRecordingResult(_ combination: HotkeyCombination, for mapping: AppSpaceHotkeyMapping) {
    if let action = HotkeyConflictDetector.actionUsing(combination, excludingAppMappingID: mapping.id) {
      NSSound.beep()
      setStatus("Shortcut already used for \(action).", color: .systemRed)
      return
    }

    store.updateHotkey(combination, for: mapping.id)
    setStatus("Updated \(mapping.displayName) shortcut.", color: .labelColor)
  }
}
