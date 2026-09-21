"""Read-only Ghidra audit of Skyrim render boundaries; never modifies the program."""
import json
import os

from ghidra.app.decompiler import DecompInterface

needles = [x.lower() for x in os.environ.get(
    "REMIX_AUDIT_SYMBOLS", "Main::Render,BSShaderAccumulator::,BSBatchRenderer::,ImageSpaceManager::,BSGraphics::Renderer::"
).split(",") if x]
base = currentProgram.getImageBase()
result = {
    "program": currentProgram.getDomainFile().getPathname(),
    "executable": currentProgram.getExecutablePath(),
    "sha256": currentProgram.getExecutableSHA256(),
    "imageBase": str(base),
    "symbols": [],
    "functions": [],
    "indirectCalls": [],
    "displacements": [],
    "data": [],
}
for value in os.environ.get("REMIX_AUDIT_DATA", "").split(","):
    if not value:
        continue
    import struct
    address = base.add(int(value, 0))
    raw = bytes(int(x) & 255 for x in getBytes(address, 4))
    refs = []
    for ref in currentProgram.getReferenceManager().getReferencesTo(address):
        function = getFunctionContaining(ref.getFromAddress())
        refs.append({"from": str(ref.getFromAddress()), "type": str(ref.getReferenceType()),
                     "function": function.getName(True) if function else None,
                     "entry": str(function.getEntryPoint()) if function else None})
    result["data"].append({"rva": value, "address": str(address), "float32": struct.unpack("<f", raw)[0],
                           "uint32": struct.unpack("<I", raw)[0], "references": refs})
for symbol in currentProgram.getSymbolTable().getSymbolIterator(True):
    name = symbol.getName(True)
    if any(needle in name.lower() for needle in needles):
        result["symbols"].append({"name": name, "address": str(symbol.getAddress()), "type": str(symbol.getSymbolType())})

scan_range = os.environ.get("REMIX_AUDIT_SCAN", "")
if scan_range:
    start, end = [int(x, 0) for x in scan_range.split(":")]
    for function in currentProgram.getFunctionManager().getFunctions(base.add(start), True):
        if function.getEntryPoint().subtract(base) >= end:
            break
        recent = []
        for instruction in currentProgram.getListing().getInstructions(function.getBody(), True):
            recent.append("{} {}".format(instruction.getAddress(), instruction))
            recent = recent[-10:]
            if (instruction.getFlowType().isCall() or instruction.getFlowType().isJump()) and len(instruction.getFlows()) == 0:
                result["indirectCalls"].append({"function": function.getName(True),
                    "entry": str(function.getEntryPoint()), "address": str(instruction.getAddress()),
                    "instruction": str(instruction), "preceding": list(recent)})

if os.environ.get("REMIX_AUDIT_DISPLACEMENTS"):
    import struct
    from jpype import JArray, JByte
    memory = currentProgram.getMemory()
    for value in os.environ["REMIX_AUDIT_DISPLACEMENTS"].split(","):
        pattern = JArray(JByte)(struct.pack("<I", int(value, 0)))
        for block in memory.getBlocks():
            if not block.isExecute() or not block.isInitialized():
                continue
            cursor = block.getStart()
            while cursor.compareTo(block.getEnd()) <= 0:
                found = memory.findBytes(cursor, block.getEnd(), pattern, None, True, monitor)
                if found is None:
                    break
                instruction = currentProgram.getListing().getInstructionContaining(found)
                if instruction is not None:
                    function = getFunctionContaining(found)
                    result["displacements"].append({"value": value, "address": str(instruction.getAddress()),
                        "instruction": str(instruction), "function": function.getName(True) if function else None,
                        "entry": str(function.getEntryPoint()) if function else None})
                cursor = found.add(1)

decompiler = DecompInterface()
decompiler.openProgram(currentProgram)
try:
    requested = []
    for entry in os.environ.get("REMIX_AUDIT_RVAS", "").split(","):
        if ":" in entry:
            start, end = [int(x, 0) for x in entry.split(":")]
            for function in currentProgram.getFunctionManager().getFunctions(base.add(start), True):
                rva = function.getEntryPoint().subtract(base)
                if rva >= end:
                    break
                requested.append(hex(rva))
        else:
            requested.append(entry)
    for value in requested:
        if not value:
            continue
        address = base.add(int(value, 0))
        function = getFunctionAt(address) or getFunctionContaining(address)
        if function is None:
            result["functions"].append({"requestedRva": value, "error": "No analyzed function"})
            continue
        decoded = decompiler.decompileFunction(function, 90, monitor)
        calls = []
        for instruction in currentProgram.getListing().getInstructions(function.getBody(), True):
            if instruction.getFlowType().isCall():
                calls.append({"address": str(instruction.getAddress()), "instruction": str(instruction),
                              "destinations": [str(x) for x in instruction.getFlows()]})
        result["functions"].append({
            "name": function.getName(True), "entry": str(function.getEntryPoint()),
            "size": int(function.getBody().getNumAddresses()),
            "bytes": [int(x) & 255 for x in getBytes(function.getEntryPoint(), min(64, int(function.getBody().getNumAddresses())))],
            "callers": [{"from": str(x.getFromAddress()), "type": str(x.getReferenceType())}
                        for x in currentProgram.getReferenceManager().getReferencesTo(function.getEntryPoint())],
            "calls": calls,
            "instructions": ["{} {}".format(x.getAddress(), x)
                             for x in currentProgram.getListing().getInstructions(function.getBody(), True)]
                            if os.environ.get("REMIX_AUDIT_INSTRUCTIONS") else [],
            "c": decoded.getDecompiledFunction().getC() if decoded.decompileCompleted() else decoded.getErrorMessage(),
        })
finally:
    decompiler.dispose()
with open(os.environ["REMIX_AUDIT_OUT"], "w", encoding="utf-8") as output:
    json.dump(result, output, indent=2)
print("Audited {} symbols and {} functions from {}".format(len(result["symbols"]), len(result["functions"]), result["program"]))
