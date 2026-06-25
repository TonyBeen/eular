# reflection 学习入口（C++11 最小可用版）

这个目录提供一个学习导向的最小运行时反射实现，目标是：

- 能注册类型
- 能读写属性
- 能调用方法（含重载匹配）
- 能调用构造函数
- 能通过统一入口析构反射构造出的对象
- 不依赖外部 any / variant 容器

不追求完整框架能力，便于先理解核心机制。

## 目录说明

- mini_reflect.h: 最小运行时反射核心实现
- type_info.h: 编译期 traits 入口（与运行时反射并存）
- examples/mini_reflect_example.cpp: 成功路径示例
- examples/mini_reflect_debug_example.cpp: 失败路径调试示例
- test/reflection_runtime_test.cc: 运行时断言测试
- test/reflection_static_assert_test.cc: 编译期静态断言测试

## 一键构建

在 reflection 目录执行：

```bash
cmake -S . -B build -DREFLECTION_BUILD_EXAMPLES=ON -DREFLECTION_BUILD_TESTS=ON
cmake --build build -j4
```

## 运行示例

### 1) 成功路径示例

```bash
./build/examples/reflection_mini_example
```

你会看到属性读写、方法调用、重载调用和析构都成功的输出。

### 2) 调试路径示例

```bash
./build/examples/reflection_mini_debug_example
```

这个示例会稳定演示两类常见错误：

- 构造参数不匹配
- 方法重载歧义

## 运行测试

```bash
ctest --test-dir build --output-on-failure
```

## 最小 API 速览

### 注册

```cpp
eular::reflection::class_<Person>("Person")
    .property("age", &Person::age)
    .method("add", static_cast<int (Person::*)(int) const>(&Person::add))
    .method("add", static_cast<double (Person::*)(double) const>(&Person::add))
    .constructor<>()
    .constructor<int, double>();
```

### 查找类型

```cpp
auto info = eular::reflection::Registry::instance().get("Person");
```

### 构造

```cpp
eular::reflection::Value obj;
std::string err;
std::vector<eular::reflection::Value> args;
args.push_back(eular::reflection::Value(20));
args.push_back(eular::reflection::Value(1.5));
bool ok = info->invoke_ctor(args, obj, err);
```

### 属性读写

```cpp
auto prop = info->find_property("age");
eular::reflection::Value value;
prop->get(person_ptr, value, err);
prop->set(person_ptr, eular::reflection::Value(25), err);
```

### 方法调用（自动选重载）

```cpp
auto method = info->find_method("add");
eular::reflection::Value out;
std::vector<eular::reflection::Value> margs;
margs.push_back(eular::reflection::Value(3));
method->invoke_best(person_ptr, margs, out, err);
```

### 析构

```cpp
info->destroy(obj, err);
```

## 重载匹配规则（当前学习版）

invoke_best 的匹配顺序：

1. 先按方法名匹配重载组
2. 再按参数个数过滤
3. 再按参数类型匹配打分

打分策略：

- 精确类型匹配：加 0 分
- 可转换匹配：加 1 分
- 不能转换：该重载淘汰

最终选择分数最低的重载；若最低分有多个，返回 overload ambiguous。

## 常见报错说明

### constructor invoke failed: no overload matches args

含义：构造参数数量或类型不匹配。

排查：

- 检查 constructor<...>() 的参数列表
- 检查 invoke_ctor 传参顺序和类型

### method invoke failed: overload ambiguous

含义：有多个重载都能匹配，且分数相同。

排查：

- 用更明确的参数类型
- 临时减少一个重载验证路径

### property set failed: type mismatch for property

含义：传入 Value 无法转换为属性类型。

排查：

- 检查属性真实类型
- 先在调用方显式构造正确类型的 Value

### destroy failed: instance type mismatch

含义：destroy 的 Value 不是该 TypeInfo 对应的对象指针。

排查：

- 确保对象由同一个类型的 invoke_ctor 产出
- 不要把其他类型的 Value 传给 destroy

## 设计取舍（当前版本）

为了便于学习，当前刻意保持简化：

- 不做继承反射
- 不做序列化/反序列化
- 不做并发注册
- 不做完整隐式转换系统

后续扩展建议顺序：

1. 构造/方法匹配规则细化（更多权重维度）
2. 析构与生命周期管理增强（对象池或自定义销毁器）
3. 元数据 attributes 支持
4. 继承关系与基类查找

## 如何学习到接近成熟反射库

如果目标是接近 RTTR、mirrow 这类成熟反射库，不建议一上来追求“大而全”。

更好的学习顺序是：

1. 先把“能用”做完整
2. 再把“类型系统”做扎实
3. 再把“调用与生命周期”做稳定
4. 最后再做“元数据、继承、序列化”这类扩展能力

也就是说，成熟反射库的核心不是接口看起来多，而是下面这几层都要足够稳。

## 第一阶段：把当前最小版补成真正可用

这一阶段的目标不是增加很多功能，而是把现有能力补齐到“可以持续扩展”。

建议先做这 6 件事：

1. 支持属性的 getter/setter 注册，而不只是成员指针
2. 支持静态方法注册
3. 支持自由函数注册
4. 支持 const / 非 const 对象区分调用
5. 支持更清晰的错误码，而不只是字符串
6. 给注册 API 增加重复注册检测

为什么这一步重要：

- 成熟库不会强依赖 public 成员变量暴露
- 很多真实项目里的属性本质上是私有字段 + getter/setter
- 自由函数和静态函数也是反射系统里常见的一等公民

## 第二阶段：补运行时类型系统

成熟反射库和“教学版”的最大差别，往往不在 method() 有多少重载，而在类型系统是否完整。

建议按这个顺序补：

1. type 的更多查询能力
2. 类型分类能力
3. 指针 / 引用 / const / 数组 / 枚举识别
4. 构造类型与裸类型之间的归一化

至少应具备下面这些查询接口：

- is_class()
- is_arithmetic()
- is_pointer()
- is_const()
- is_enum()
- get_raw_type()
- get_name()
- get_sizeof()

为什么这一步重要：

- 方法匹配、属性赋值、序列化扩展，最终都依赖可靠的类型系统
- 没有类型归一化，很多匹配逻辑会越来越乱

## 第三阶段：把重载解析做得更像成熟库

当前版本的重载匹配规则比较简单：精确匹配优先，其次是可转换匹配。

成熟库通常会进一步区分：

1. 完全精确匹配
2. const 引用绑定
3. 数值提升
4. 普通数值转换
5. 用户自定义转换

也就是说，不同“能转”的质量并不一样。

建议把当前单一 score 扩展成更细的分层权重，例如：

- 精确匹配：0
- cv/ref 调整：1
- 窄范围标准转换：2
- 普通数值转换：3
- 需要 Value 内部转换：4

这样做之后：

- 重载歧义会减少
- 调用行为更稳定
- 更接近真实 C++ 重载决议的直觉

## 第四阶段：补对象生命周期模型

当前实现里，构造统一返回 T*，再由 destroy 删除。这适合学习，但离成熟库还有距离。

成熟一点的设计，建议逐步补成：

1. constructor 返回对象实例或统一包装对象
2. 支持栈对象引用包装
3. 支持非 owning 引用
4. 支持自定义销毁器
5. 支持工厂函数构造

为什么这一步重要：

- 真实工程里，很多对象不是 new 出来的
- 有些对象来自对象池、单例、外部框架生命周期
- 反射系统如果只会 delete，很容易和真实项目冲突

## 第五阶段：支持私有成员的成熟注册方式

成熟库一般不会要求“把字段改成 public 才能注册”。

建议你优先做这两类能力：

1. property(name, getter, setter)
2. property_readonly(name, getter)

这样私有字段就可以通过公开接口暴露给反射系统，例如：

```cpp
class Person
{
public:
    int get_age() const { return age_; }
    void set_age(int v) { age_ = v; }

private:
    int age_;
};

eular::reflection::class_<Person>("Person")
    .property("age", &Person::get_age, &Person::set_age);
```

这比直接注册私有字段更接近成熟库的设计习惯，因为它保留了封装边界。

## 第六阶段：增加元数据系统

成熟反射库通常不只关心“能不能调”，还关心“这是什么”。

典型需求包括：

- UI 面板显示名
- 序列化字段别名
- 是否只读
- 数值范围
- 分类标签
- 自定义注释

建议后续让 type / property / method 都能挂 attributes，例如：

```cpp
.property("age", &Person::get_age, &Person::set_age)
    .attribute("display_name", "年龄")
    .attribute("min", 0)
    .attribute("max", 150);
```

这是从“反射能调用”走向“反射可驱动工具”的关键一步。

## 第七阶段：补继承与向上查找

如果要接近成熟库，继承支持基本绕不过去。

至少建议支持：

1. 注册基类关系
2. 通过基类视角访问派生对象
3. 查找属性/方法时向基类回溯
4. 类型判断：is_derived_from

这一步完成后，反射系统才更接近真实面向对象代码的使用方式。

## 第八阶段：补枚举、容器、工厂函数

这些能力往往不是最先做，但非常常见：

1. 枚举名字和值互转
2. 容器识别与遍历适配
3. 工厂函数反射
4. 静态常量或静态属性暴露

它们会显著提升系统的实用性。

## 推荐学习路径

如果你想边学边做，我建议按下面顺序推进当前代码：

1. 先实现 getter/setter 版 property
2. 再实现 static method 和 free function 注册
3. 然后重写 overload score 规则
4. 再补 attribute 元数据
5. 最后补 base class 反射

这个顺序的好处是：

- 每一步都能看到直接收益
- 不会过早陷入复杂继承模型
- 学到的内容和成熟库核心机制高度一致

## 你现在最该重点理解什么

如果只抓最关键的 4 个点，建议优先理解：

1. 注册信息最终如何落到 TypeInfo / PropertyInfo / MethodInfo 上
2. Value 为什么是整个运行时反射系统的桥梁
3. 重载解析为什么本质上是“类型系统 + 转换规则”的问题
4. 成熟库为什么更偏好 getter/setter，而不是直接暴露字段

把这 4 点吃透后，再去读 RTTR 或 mirrow 的实现，会轻松很多。
