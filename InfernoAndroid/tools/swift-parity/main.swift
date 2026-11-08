import Foundation
// Linux scaffolding only; the three reference implementations are unmodified.
func L(_ text: String, _ args: CVarArg...) -> String { String(format: text, arguments: args) }
func autoreleasepool<T>(invoking body: () throws -> T) rethrows -> T { try body() }
let fixtures = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let output = URL(fileURLWithPath: CommandLine.arguments[2], isDirectory: true)
try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
let shsh = try Data(contentsOf: fixtures.appendingPathComponent("ticket.shsh2"))
let identity = try PropertyListSerialization.propertyList(from: Data(contentsOf: fixtures.appendingPathComponent("identity.plist")), options: [], format: nil) as! [String: Any]
let ap = try Ticket.forgeAP(shsh: shsh, identity: identity)
let sep = try Ticket.forgeSEP(shsh: shsh, identity: identity)
try Data(ap).write(to: output.appendingPathComponent("ap.der"))
try Data(sep).write(to: output.appendingPathComponent("sep.der"))
let payload = try IMG4.readIM4P([UInt8](Data(contentsOf: fixtures.appendingPathComponent("payload.im4p"))))
try Data(try IMG4.make(payload: payload, ticket: ap)).write(to: output.appendingPathComponent("personalized.img4"))
try RestoreRamdisk.job.write(to: output.appendingPathComponent("job.plist"))
try RestoreRamdisk.build(stock: fixtures.appendingPathComponent("ramdisk.im4p"), into: output.appendingPathComponent("ramdisk.im4p"))
print("Swift synthetic reference outputs written to \(output.path)")
