import AppKit
import Combine
import Foundation
import ISS

struct DetectedAppSpace: Equatable {
  let index: UInt32
  let spaceCount: UInt32
  let spaceID: UInt64
  let displayID: String
  let ownerPID: Int32
  let ownerName: String
  let bundleIdentifier: String?
  let isFullscreen: Bool

  var displayName: String {
    if let app = runningApplication {
      return app.localizedName ?? ownerName
    }
    return ownerName
  }

  var runningApplication: NSRunningApplication? {
    guard ownerPID > 0 else { return nil }
    return NSRunningApplication(processIdentifier: pid_t(ownerPID))
  }
}

struct AppSpaceHotkeyMapping: Codable, Identifiable, Equatable {
  var id: UUID
  var isEnabled: Bool
  var displayName: String
  var bundleIdentifier: String?
  var ownerName: String
  var hotkey: HotkeyCombination?

  var registrationKey: String {
    "appSpace.\(id.uuidString)"
  }

  func matches(_ space: DetectedAppSpace) -> Bool {
    if let bundleIdentifier, let candidate = space.bundleIdentifier {
      return bundleIdentifier == candidate
    }
    return ownerName == space.ownerName || displayName == space.displayName
  }
}

final class AppSpaceHotkeyStore: ObservableObject {
  static let shared = AppSpaceHotkeyStore()

  @Published private(set) var mappings: [AppSpaceHotkeyMapping]

  private let defaults: UserDefaults
  private let defaultsKey = "appSpaceHotkeyMappings"

  init(defaults: UserDefaults = .standard) {
    self.defaults = defaults
    mappings = defaults.appSpaceMappings(forKey: defaultsKey)
  }

  func addMapping(for space: DetectedAppSpace) {
    let bundleIdentifier = space.bundleIdentifier
    if mappings.contains(where: { mapping in
      if let bundleIdentifier, mapping.bundleIdentifier == bundleIdentifier {
        return true
      }
      return mapping.bundleIdentifier == nil && mapping.ownerName == space.ownerName
    }) {
      return
    }

    mappings.append(
      AppSpaceHotkeyMapping(
        id: UUID(),
        isEnabled: true,
        displayName: space.displayName,
        bundleIdentifier: bundleIdentifier,
        ownerName: space.ownerName,
        hotkey: nil
      ))
    save()
  }

  func updateHotkey(_ hotkey: HotkeyCombination?, for id: UUID) {
    guard let index = mappings.firstIndex(where: { $0.id == id }) else { return }
    mappings[index].hotkey = hotkey
    save()
  }

  func setEnabled(_ enabled: Bool, for id: UUID) {
    guard let index = mappings.firstIndex(where: { $0.id == id }) else { return }
    mappings[index].isEnabled = enabled
    save()
  }

  func removeMapping(id: UUID) {
    mappings.removeAll { $0.id == id }
    save()
  }

  private func save() {
    defaults.setAppSpaceMappings(mappings, forKey: defaultsKey)
  }
}

enum AppSpaceResolver {
  static func detectedSpaces() -> [DetectedAppSpace] {
    guard let unmanaged = iss_copy_cursor_display_app_spaces() else {
      return []
    }

    let array = unmanaged.takeRetainedValue() as NSArray
    return array.compactMap { item in
      guard let dict = item as? NSDictionary,
            let index = dict["index"] as? NSNumber,
            let spaceCount = dict["spaceCount"] as? NSNumber,
            let spaceID = dict["spaceID"] as? NSNumber,
            let ownerPID = dict["ownerPID"] as? NSNumber,
            let ownerName = dict["ownerName"] as? String
      else {
        return nil
      }

      let pid = ownerPID.int32Value
      let app = pid > 0 ? NSRunningApplication(processIdentifier: pid_t(pid)) : nil
      return DetectedAppSpace(
        index: index.uint32Value,
        spaceCount: spaceCount.uint32Value,
        spaceID: spaceID.uint64Value,
        displayID: dict["displayID"] as? String ?? "",
        ownerPID: pid,
        ownerName: ownerName,
        bundleIdentifier: app?.bundleIdentifier,
        isFullscreen: (dict["isFullscreen"] as? NSNumber)?.boolValue ?? false
      )
    }
  }

  static func resolveTargetIndex(for mapping: AppSpaceHotkeyMapping) -> UInt32? {
    detectedSpaces().first(where: { mapping.matches($0) })?.index
  }
}

enum HotkeyConflictDetector {
  static func actionUsing(
    _ combination: HotkeyCombination,
    excludingIdentifier excludedIdentifier: HotkeyIdentifier? = nil,
    excludingAppMappingID excludedID: UUID? = nil
  ) -> String? {
    let hotkeyStore = HotkeyStore.shared
    for identifier in HotkeyIdentifier.allCases
    where identifier != excludedIdentifier && hotkeyStore.isEnabled(identifier)
      && hotkeyStore.combination(for: identifier) == combination
    {
      return identifier.displayName
    }

    for mapping in AppSpaceHotkeyStore.shared.mappings
    where mapping.id != excludedID && mapping.isEnabled && mapping.hotkey == combination {
      return mapping.displayName
    }

    return nil
  }
}

extension UserDefaults {
  fileprivate func appSpaceMappings(forKey key: String) -> [AppSpaceHotkeyMapping] {
    guard let data = data(forKey: key) else { return [] }
    return (try? JSONDecoder().decode([AppSpaceHotkeyMapping].self, from: data)) ?? []
  }

  fileprivate func setAppSpaceMappings(_ mappings: [AppSpaceHotkeyMapping], forKey key: String) {
    if let data = try? JSONEncoder().encode(mappings) {
      set(data, forKey: key)
    }
  }
}
