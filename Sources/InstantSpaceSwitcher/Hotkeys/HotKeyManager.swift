import AppKit
import Carbon

class HotKeyManager {
  static let shared = HotKeyManager()

  private struct Registration {
    let id: UInt32
    var reference: EventHotKeyRef?
  }

  private var handlers: [UInt32: () -> Void] = [:]
  private var registrations: [String: Registration] = [:]
  private var currentId: UInt32 = 1

  private init() {
    installEventHandler()
  }

  func register(
    identifier: HotkeyIdentifier, combination: HotkeyCombination, handler: @escaping () -> Void
  ) {
    register(key: identifier.rawValue, combination: combination, handler: handler)
  }

  func register(
    key: String, combination: HotkeyCombination, handler: @escaping () -> Void
  ) {
    unregister(key: key)

    let id = currentId
    currentId &+= 1

    var hotKeyRef: EventHotKeyRef?
    let hotKeyID = EventHotKeyID(signature: 0x1111, id: id)
    let status = RegisterEventHotKey(
      combination.keyCode, combination.modifiers, hotKeyID, GetEventDispatcherTarget(), 0,
      &hotKeyRef)

    guard status == noErr else {
      print("Failed to register hotkey for \(key) status=\(status)")
      return
    }

    handlers[id] = handler
    registrations[key] = Registration(id: id, reference: hotKeyRef)
  }

  func unregister(identifier: HotkeyIdentifier) {
    unregister(key: identifier.rawValue)
  }

  func unregister(key: String) {
    guard let registration = registrations.removeValue(forKey: key) else { return }
    handlers.removeValue(forKey: registration.id)
    if let reference = registration.reference {
      UnregisterEventHotKey(reference)
    }
  }

  func unregisterAll() {
    for (_, registration) in registrations {
      if let reference = registration.reference {
        UnregisterEventHotKey(reference)
      }
    }
    registrations.removeAll()
    handlers.removeAll()
  }

  private func installEventHandler() {
    var eventSpec = EventTypeSpec(
      eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))

    InstallEventHandler(
      GetEventDispatcherTarget(),
      { (_, event, _) -> OSStatus in
        var hotKeyID = EventHotKeyID()
        GetEventParameter(
          event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID), nil,
          MemoryLayout<EventHotKeyID>.size, nil, &hotKeyID)

        if let handler = HotKeyManager.shared.handlers[hotKeyID.id] {
          handler()
        }

        return noErr
      }, 1, &eventSpec, nil, nil)
  }
}
