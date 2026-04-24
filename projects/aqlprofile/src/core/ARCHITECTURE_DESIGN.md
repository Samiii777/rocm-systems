# Architecture Abstraction Design

## Thread Safety Guarantees

### ArchitectureRegistry
- **Thread-safe**: Yes, fully thread-safe for all operations
- **Concurrency model**: Uses `std::shared_mutex` for read-write locking
  - `Register()`: Exclusive lock (single writer)
  - `Lookup()`, `GetExact()`, `GetRegisteredPrefixes()`: Shared lock (multiple readers)
  - `Clear()`: Exclusive lock (single writer)
- **Initialization**: Call `Register()` during library initialization before multi-threaded use
- **Lookups**: Safe to call from multiple threads concurrently with no contention

### HardwareArchitecture
- **Thread-safe**: Yes, all methods are const and read-only
- **Concurrency model**: Immutable after construction
- **Usage**: Safe to call any method from multiple threads
- **Lifetime**: Managed by `std::unique_ptr`, single owner

### HardwareConfig
- **Thread-safe**: Yes, immutable POD struct
- **Concurrency model**: Read-only access
- **Usage**: Safe to read from multiple threads

### RegisterSchema  
- **Thread-safe**: Yes, immutable after construction
- **Concurrency model**: Read-only access via const methods
- **Usage**: Safe to read from multiple threads

## Ownership Semantics

### CreateArchitectureForAgent()
```cpp
std::unique_ptr<HardwareArchitecture> CreateArchitectureForAgent(const AgentInfo* agent_info);
```
- **Returns**: `std::unique_ptr` - caller takes ownership
- **Lifetime**: Caller is responsible for managing lifetime
- **Typical usage**: Pass to `Pm4FactoryAdapter` constructor which takes ownership

### Pm4FactoryAdapter
```cpp
explicit Pm4FactoryAdapter(std::unique_ptr<HardwareArchitecture> architecture);
```
- **Takes ownership**: Yes, via move semantics
- **Lifetime**: Architecture deleted when adapter is destroyed
- **Builder ownership**: Adapter creates and owns all builders (cmd, pmc, spm, sqtt)

### ArchitectureRegistry::Register()
```cpp
void Register(const std::string& gfxip_prefix,
              std::unique_ptr<HardwareArchitecture> architecture);
```
- **Takes ownership**: Yes, architecture is moved into registry
- **Lifetime**: Architecture lives as long as registry (typically program lifetime)
- **Deletion**: Automatic via `unique_ptr` when cleared or registry destroyed

## Error Handling Strategy

### Lookup Failures
- **Registry::Lookup()**: Returns `nullptr` and logs error to stderr
- **Client code**: Must check for nullptr before dereferencing
- **Logging**: Includes list of supported architectures for debugging

### Integer Overflow
- **GetBytesNeededForBlock()**: Throws `std::overflow_error` if calculation overflows
- **Client code**: Must catch overflow_error or let it propagate
- **Prevention**: Validates intermediate calculations before multiplication

### Invalid Input
- **BlockInfoMap::Find()**: Returns `UINT32_MAX` for invalid/null names
- **String validation**: Bounded length check (MAX_BLOCK_NAME_LEN = 64)
- **Null checks**: All pointer inputs validated before use

### Memory Allocation
- **std::make_unique**: May throw `std::bad_alloc` on OOM
- **Client code**: Should handle bad_alloc or let it terminate (common pattern)

## Performance Characteristics

### Registry Lookup
- **Complexity**: O(N) where N = number of registered architectures (typically < 10)
- **Optimization**: Shared lock allows concurrent reads without contention
- **Hot path**: Yes, called per-agent on first use
- **Caching**: Client should cache architecture pointer after first lookup

### GetBytesNeededForBlock
- **Complexity**: O(1) arithmetic with overflow checks
- **Overhead**: Additional integer comparisons for safety
- **Hot path**: Yes, called per block allocation
- **Recommendation**: Results should be cached when possible

### String Comparison (BlockInfoMap::Find)
- **Complexity**: O(N*M) where N = blocks, M = name length (bounded)
- **Optimization**: Early termination on length mismatch
- **Hot path**: Yes, called per event lookup
- **Bounded**: Maximum 64-character comparison prevents DoS

## Migration Status

### Complete
- ✅ Architecture abstraction layer (HardwareArchitecture, HardwareConfig, RegisterSchema)
- ✅ Registry system with thread-safe lookup
- ✅ Ownership via unique_ptr
- ✅ Overflow protection in buffer calculations
- ✅ Input validation in string operations
- ✅ Error logging for debugging

### In Progress  
- ⚠️ PMC/SPM/SQTT builder integration with new abstractions
- ⚠️ RegisterSchema population and usage in builders
- ⚠️ Complete removal of old factory pattern

### Not Started
- ❌ Architecture-specific builder templates using RegisterSchema
- ❌ Compile-time to runtime builder transition
- ❌ Full test coverage of multi-threaded scenarios
- ❌ Performance benchmarks vs old implementation
