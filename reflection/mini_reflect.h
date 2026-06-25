#ifndef __EULAR_REFLECTION_MINI_REFLECT_H__
#define __EULAR_REFLECTION_MINI_REFLECT_H__

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eular
{
    namespace reflection
    {

        // C++11 版本的 index_sequence 实现，用于展开参数包。
        template <size_t... Is>
        struct index_sequence
        {
        };

        template <size_t N, size_t... Is>
        struct make_index_sequence_impl : make_index_sequence_impl<N - 1, N - 1, Is...>
        {
        };

        template <size_t... Is>
        struct make_index_sequence_impl<0, Is...>
        {
            typedef index_sequence<Is...> type;
        };

        template <size_t N>
        using make_index_sequence = typename make_index_sequence_impl<N>::type;

        // Type 是运行时类型句柄，用来表示“某个具体 C++ 类型”。
        // 这里不保存对象，只保存类型身份，便于做比较、查询和匹配。
        class Type
        {
        public:
            Type() : index_(typeid(void)) {}
            explicit Type(const std::type_info &info) : index_(info) {}

            template <typename T>
            static Type get()
            {
                return Type(typeid(T));
            }

            bool operator==(const Type &other) const
            {
                return index_ == other.index_;
            }

            bool operator!=(const Type &other) const
            {
                return !(*this == other);
            }

            bool is_valid() const
            {
                return index_ != std::type_index(typeid(void));
            }

            std::string name() const
            {
                return index_.name();
            }

        private:
            std::type_index index_;
        };

        // Value 是运行时值容器，承担反射系统里的“桥梁”角色。
        // 方法调用、属性读写、构造参数，都会先进入 Value，再按目标类型取出或转换。
        class Value
        {
        public:
            Value()
            {
            }

            Value(const Value &other)
                : storage_(other.storage_ ? other.storage_->clone() : std::shared_ptr<HolderBase>())
            {
            }

            Value &operator=(const Value &other)
            {
                if (this != &other)
                {
                    storage_ = other.storage_ ? other.storage_->clone() : std::shared_ptr<HolderBase>();
                }
                return *this;
            }

                        // 用任意可拷贝类型构造一个 Value。
                        // 通过类型擦除把具体类型藏到 Holder<T> 里。
                        template <typename T>
            Value(const T &value,
                  typename std::enable_if<!std::is_same<typename std::decay<T>::type, Value>::value, int>::type = 0)
                : storage_(new Holder<typename std::decay<T>::type>(value))
            {
            }

            bool is_valid() const
            {
                return static_cast<bool>(storage_);
            }

            void clear()
            {
                storage_.reset();
            }

            Type get_type() const
            {
                return storage_ ? storage_->get_type() : Type();
            }

            // 判断当前保存的值是不是指定类型。
            // 反射调用前通常会先做这个检查，避免错误类型直接 get_value 触发异常。
            template <typename T>
            bool is_type() const
            {
                return get_type() == Type::get<typename std::decay<T>::type>();
            }

            // 取出保存的值。类型不匹配时抛出 bad_cast。
            // 这和 dynamic_cast 的失败语义保持一致，方便在上层统一处理。
            template <typename T>
            typename std::decay<T>::type get_value() const
            {
                typedef typename std::decay<T>::type value_type;
                if (!storage_)
                {
                    throw std::bad_cast();
                }

                Holder<value_type> *holder = dynamic_cast<Holder<value_type> *>(storage_.get());
                if (!holder)
                {
                    throw std::bad_cast();
                }

                return holder->value;
            }

            // 尝试把当前值转换成目标类型。
            // 当前版本只对基础算术类型提供转换支持，足够覆盖学习版重载匹配。
            bool convert(const Type &target)
            {
                if (!storage_)
                {
                    return false;
                }

                Value out;
                if (!storage_->convert_to(target, out))
                {
                    return false;
                }

                *this = out;
                return true;
            }

        private:
            // HolderBase 是类型擦除的抽象基类。
            // 反射系统只和它打交道，不需要知道里面真实存的是哪个类型。
            struct HolderBase
            {
                virtual ~HolderBase() {}
                virtual Type get_type() const = 0;
                virtual std::shared_ptr<HolderBase> clone() const = 0;
                virtual bool convert_to(const Type &target, Value &out) const = 0;
            };

            // 具体类型的持有者。
            // 每个 Value 都会落到一个 Holder<T> 上，保存真实值并提供 clone/convert 能力。
            template <typename T>
            struct Holder : HolderBase
            {
                explicit Holder(const T &v) : value(v) {}

                virtual Type get_type() const
                {
                    return Type::get<T>();
                }

                virtual std::shared_ptr<HolderBase> clone() const
                {
                    return std::shared_ptr<HolderBase>(new Holder<T>(value));
                }

                virtual bool convert_to(const Type &target, Value &out) const
                {
                    if (target == Type::get<T>())
                    {
                        out = Value(value);
                        return true;
                    }

                    return Value::TryConvert(value, target, out);
                }

                T value;
            };

            // 非算术类型默认不支持自动转换。
            // 学习版先保持保守，避免引入过重的隐式转换规则。
            template <typename T>
            static typename std::enable_if<!std::is_arithmetic<T>::value, bool>::type TryConvert(const T &, const Type &, Value &)
            {
                return false;
            }

            // 算术类型之间的基础转换规则。
            // 这里按“能否转换”来判断，不做复杂的转换优先级计算。
            template <typename T>
            static typename std::enable_if<std::is_arithmetic<T>::value, bool>::type TryConvert(const T &value, const Type &target, Value &out)
            {
                if (target == Type::get<bool>())
                {
                    out = Value(static_cast<bool>(value));
                    return true;
                }
                if (target == Type::get<char>())
                {
                    out = Value(static_cast<char>(value));
                    return true;
                }
                if (target == Type::get<signed char>())
                {
                    out = Value(static_cast<signed char>(value));
                    return true;
                }
                if (target == Type::get<unsigned char>())
                {
                    out = Value(static_cast<unsigned char>(value));
                    return true;
                }
                if (target == Type::get<short>())
                {
                    out = Value(static_cast<short>(value));
                    return true;
                }
                if (target == Type::get<unsigned short>())
                {
                    out = Value(static_cast<unsigned short>(value));
                    return true;
                }
                if (target == Type::get<int>())
                {
                    out = Value(static_cast<int>(value));
                    return true;
                }
                if (target == Type::get<unsigned int>())
                {
                    out = Value(static_cast<unsigned int>(value));
                    return true;
                }
                if (target == Type::get<long>())
                {
                    out = Value(static_cast<long>(value));
                    return true;
                }
                if (target == Type::get<unsigned long>())
                {
                    out = Value(static_cast<unsigned long>(value));
                    return true;
                }
                if (target == Type::get<long long>())
                {
                    out = Value(static_cast<long long>(value));
                    return true;
                }
                if (target == Type::get<unsigned long long>())
                {
                    out = Value(static_cast<unsigned long long>(value));
                    return true;
                }
                if (target == Type::get<float>())
                {
                    out = Value(static_cast<float>(value));
                    return true;
                }
                if (target == Type::get<double>())
                {
                    out = Value(static_cast<double>(value));
                    return true;
                }
                if (target == Type::get<long double>())
                {
                    out = Value(static_cast<long double>(value));
                    return true;
                }

                return false;
            }

            std::shared_ptr<HolderBase> storage_;
        };

        // 生成一段参数类型说明，用于错误信息。
        // 当前版本优先保证可读性，类型名直接用 Type::name() 输出。
        inline std::string BuildArgTypeList(const std::vector<Type> &types)
        {
            std::ostringstream oss;
            for (size_t i = 0; i < types.size(); ++i)
            {
                if (i != 0)
                {
                    oss << ", ";
                }
                oss << types[i].name();
            }
            return oss.str();
        }

        // 统一封装 Value 转换，避免上层重复写 try/catch。
        inline bool ConvertValueToType(Value &value, const Type &target)
        {
            try
            {
                return value.convert(target);
            }
            catch (...)
            {
                return false;
            }
        }

        // PropertyInfo 描述一个可反射属性。
        // get/set 都是运行时回调，真正的访问逻辑由注册时绑定。
        struct PropertyInfo
        {
            PropertyInfo(const std::string &n, const Type &t)
                : name(n), value_type(t) {}

            std::string name;
            Type value_type;

            std::function<bool(void *, Value &, std::string &)> get;
            std::function<bool(void *, const Value &, std::string &)> set;
        };

        // MethodOverload 表示一个具体签名的方法重载。
        // 同名方法会被聚合到 MethodInfo 里，由 invoke_best 做匹配。
        struct MethodOverload
        {
            MethodOverload(const std::string &n,
                           const Type &ret,
                           const std::vector<Type> &args)
                : name(n), return_type(ret), arg_types(args) {}

            std::string name;
            Type return_type;
            std::vector<Type> arg_types;

            std::function<bool(void *, const std::vector<Value> &, Value &, std::string &)> invoke;
        };

        // MethodInfo 是一个“方法重载组”。
        // 它不关心某个签名如何调用，只负责从多条 overload 里挑出最合适的一条。
        struct MethodInfo
        {
            std::string name;
            std::vector<MethodOverload> overloads;

            // 按参数数量和类型分数选择最优重载。
            // 规则很简单：精确匹配优先，能转换但非精确匹配次之，分数最低者胜出。
            bool invoke_best(void *instance,
                             const std::vector<Value> &args,
                             Value &out,
                             std::string &err) const
            {
                if (!instance)
                {
                    err = "method invoke failed: instance is null";
                    return false;
                }

                try
                {
                    const MethodOverload *best = NULL;
                    std::vector<Value> best_converted;
                    int best_score = 0;
                    bool ambiguous = false;

                    for (size_t i = 0; i < overloads.size(); ++i)
                    {
                        const MethodOverload &ov = overloads[i];
                        if (ov.arg_types.size() != args.size())
                        {
                            continue;
                        }

                        int score = 0;
                        std::vector<Value> converted;
                        converted.reserve(args.size());

                        bool ok = true;
                        for (size_t j = 0; j < args.size(); ++j)
                        {
                            const Type expected = ov.arg_types[j];
                            const Value &given = args[j];

                            if (given.get_type() == expected)
                            {
                                converted.push_back(given);
                                continue;
                            }

                            Value temp = given;
                            if (!ConvertValueToType(temp, expected))
                            {
                                ok = false;
                                break;
                            }

                            ++score;
                            converted.push_back(temp);
                        }

                        if (!ok)
                        {
                            continue;
                        }

                        if (!best || score < best_score)
                        {
                            best = &ov;
                            best_score = score;
                            best_converted = converted;
                            ambiguous = false;
                        }
                        else if (score == best_score)
                        {
                            ambiguous = true;
                        }
                    }

                    if (!best)
                    {
                        err = "method invoke failed: no overload matches args [" + BuildArgTypeList(CollectArgTypes(args)) + "]";
                        return false;
                    }

                    if (ambiguous)
                    {
                        err = "method invoke failed: overload ambiguous for method '" + name + "'";
                        return false;
                    }

                    return best->invoke(instance, best_converted, out, err);
                }
                catch (...)
                {
                    err = "method invoke failed: internal exception";
                    return false;
                }
            }

        private:
            static std::vector<Type> CollectArgTypes(const std::vector<Value> &args)
            {
                std::vector<Type> out;
                out.reserve(args.size());
                for (size_t i = 0; i < args.size(); ++i)
                {
                    out.push_back(args[i].get_type());
                }
                return out;
            }
        };

        // ConstructorInfo 描述一个构造函数签名。
        // 与方法重载类似，构造函数也按参数匹配选择最优项。
        struct ConstructorInfo
        {
            std::vector<Type> arg_types;
            std::function<bool(const std::vector<Value> &, Value &, std::string &)> invoke;
        };

        // TypeInfo 保存一个类型的完整反射数据。
        // 它是 registry 里真正的“类型元数据容器”。
        struct TypeInfo
        {
            TypeInfo(const std::string &n, const Type &t, bool des)
                : name(n), object_type(t), is_destructible(des) {}

            std::string name;
            Type object_type;

            std::vector<PropertyInfo> properties;
            std::unordered_map<std::string, MethodInfo> methods;
            std::vector<ConstructorInfo> constructors;

            bool is_destructible;
            std::function<bool(Value &, std::string &)> destroy;

            PropertyInfo *find_property(const std::string &property_name)
            {
                for (size_t i = 0; i < properties.size(); ++i)
                {
                    if (properties[i].name == property_name)
                    {
                        return &properties[i];
                    }
                }
                return NULL;
            }

            const PropertyInfo *find_property(const std::string &property_name) const
            {
                for (size_t i = 0; i < properties.size(); ++i)
                {
                    if (properties[i].name == property_name)
                    {
                        return &properties[i];
                    }
                }
                return NULL;
            }

            MethodInfo *find_method(const std::string &method_name)
            {
                std::unordered_map<std::string, MethodInfo>::iterator it = methods.find(method_name);
                if (it == methods.end())
                {
                    return NULL;
                }
                return &it->second;
            }

            const MethodInfo *find_method(const std::string &method_name) const
            {
                std::unordered_map<std::string, MethodInfo>::const_iterator it = methods.find(method_name);
                if (it == methods.end())
                {
                    return NULL;
                }
                return &it->second;
            }

            // 从构造函数重载组里选择最优构造路径。
            // 规则与方法调用一致，方便用户形成统一的心智模型。
            bool invoke_ctor(const std::vector<Value> &args, Value &out, std::string &err) const
            {
                try
                {
                    const ConstructorInfo *best = NULL;
                    std::vector<Value> best_converted;
                    int best_score = 0;
                    bool ambiguous = false;

                    for (size_t i = 0; i < constructors.size(); ++i)
                    {
                        const ConstructorInfo &ctor = constructors[i];
                        if (ctor.arg_types.size() != args.size())
                        {
                            continue;
                        }

                        int score = 0;
                        std::vector<Value> converted;
                        converted.reserve(args.size());

                        bool ok = true;
                        for (size_t j = 0; j < args.size(); ++j)
                        {
                            const Type expected = ctor.arg_types[j];
                            const Value &given = args[j];

                            if (given.get_type() == expected)
                            {
                                converted.push_back(given);
                                continue;
                            }

                            Value temp = given;
                            if (!ConvertValueToType(temp, expected))
                            {
                                ok = false;
                                break;
                            }

                            ++score;
                            converted.push_back(temp);
                        }

                        if (!ok)
                        {
                            continue;
                        }

                        if (!best || score < best_score)
                        {
                            best = &ctor;
                            best_score = score;
                            best_converted = converted;
                            ambiguous = false;
                        }
                        else if (score == best_score)
                        {
                            ambiguous = true;
                        }
                    }

                    if (!best)
                    {
                        err = "constructor invoke failed: no overload matches args [" + BuildArgTypeList(CollectArgTypes(args)) + "]";
                        return false;
                    }

                    if (ambiguous)
                    {
                        err = "constructor invoke failed: overload ambiguous";
                        return false;
                    }

                    return best->invoke(best_converted, out, err);
                }
                catch (...)
                {
                    err = "constructor invoke failed: internal exception";
                    return false;
                }
            }

        private:
            static std::vector<Type> CollectArgTypes(const std::vector<Value> &args)
            {
                std::vector<Type> out;
                out.reserve(args.size());
                for (size_t i = 0; i < args.size(); ++i)
                {
                    out.push_back(args[i].get_type());
                }
                return out;
            }
        };

        // Registry 是全局类型注册表。
        // 学习版采用单例，方便在任意位置注册和查询反射类型。
        class Registry
        {
        public:
            static Registry &instance()
            {
                static Registry inst;
                return inst;
            }

            // 如果类型不存在，就创建一份 TypeInfo；如果已经注册过，就直接复用。
            template <typename T>
            std::shared_ptr<TypeInfo> get_or_create(const std::string &name)
            {
                std::unordered_map<std::string, std::shared_ptr<TypeInfo> >::iterator it = types_.find(name);
                if (it != types_.end())
                {
                    return it->second;
                }

                std::shared_ptr<TypeInfo> info(new TypeInfo(name, Type::get<T>(), std::is_destructible<T>::value));

                info->destroy = &DestroyInstance<T>;
                types_[name] = info;
                return info;
            }

            std::shared_ptr<TypeInfo> get(const std::string &name)
            {
                std::unordered_map<std::string, std::shared_ptr<TypeInfo> >::iterator it = types_.find(name);
                if (it == types_.end())
                {
                    return std::shared_ptr<TypeInfo>();
                }
                return it->second;
            }

        private:
            Registry() {}

            // 析构入口：只接受由当前类型构造出来的 T*。
            // 这保证 destroy 的行为简单明确，不和外部生命周期模型混在一起。
            template <typename T>
            static bool DestroyInstance(Value &instance, std::string &err)
            {
                if (!instance.is_type<T *>())
                {
                    err = "destroy failed: instance type mismatch";
                    return false;
                }

                T *ptr = instance.get_value<T *>();
                if (!ptr)
                {
                    err = "destroy failed: null instance pointer";
                    return false;
                }

                delete ptr;
                instance.clear();
                return true;
            }

            std::unordered_map<std::string, std::shared_ptr<TypeInfo> > types_;
        };

        template <typename T>
        // class_builder 是注册入口的链式构建器。
        // class_<T>("Name") 返回它，然后依次注册 property / method / constructor。
        class class_builder
        {
        public:
            explicit class_builder(const std::shared_ptr<TypeInfo> &info) : info_(info) {}

            // 直接注册成员变量属性。
            // 成员指针要求调用点具备访问权限，因此私有成员需要在类内或友元函数里注册。
            template <typename MemberType>
            class_builder &property(const std::string &name, MemberType T::*member)
            {
                PropertyInfo p(name, Type::get<typename std::decay<MemberType>::type>());

                // get 回调把对象指针转回 T，然后把成员值写入 Value。
                p.get = [member](void *instance, Value &out, std::string &err) -> bool
                {
                    if (!instance)
                    {
                        err = "property get failed: instance is null";
                        return false;
                    }

                    T *obj = static_cast<T *>(instance);
                    out = Value(obj->*member);
                    return true;
                };

                // set 回调先尝试把传入值转换成成员真实类型，再写回对象。
                p.set = [member](void *instance, const Value &value, std::string &err) -> bool
                {
                    if (!instance)
                    {
                        err = "property set failed: instance is null";
                        return false;
                    }

                    T *obj = static_cast<T *>(instance);
                    Value converted = value;
                    Type expected = Type::get<typename std::decay<MemberType>::type>();

                    if (converted.get_type() != expected)
                    {
                        if (!ConvertValueToType(converted, expected))
                        {
                            err = "property set failed: type mismatch for property";
                            return false;
                        }
                    }

                    obj->*member = converted.get_value<typename std::decay<MemberType>::type>();
                    return true;
                };

                info_->properties.push_back(p);
                return *this;
            }

            // 注册非常量成员函数。
            // 调用时会先做参数转换，再把参数展开到真实函数调用中。
            template <typename Ret, typename... Args>
            class_builder &method(const std::string &name, Ret (T::*func)(Args...))
            {
                MethodOverload ov(name,
                                  Type::get<typename std::decay<Ret>::type>(),
                                  BuildArgTypes<Args...>());
                ov.invoke = [func](void *instance,
                                   const std::vector<Value> &args,
                                   Value &out,
                                   std::string &err) -> bool
                {
                    return InvokeMember<Ret, Args...>(static_cast<T *>(instance), func, args, out, err);
                };

                MethodInfo &group = info_->methods[name];
                group.name = name;
                group.overloads.push_back(ov);
                return *this;
            }

            // 注册 const 成员函数。
            // 这类方法可以在只读对象视角下调用，适合查询类接口。
            template <typename Ret, typename... Args>
            class_builder &method(const std::string &name, Ret (T::*func)(Args...) const)
            {
                MethodOverload ov(name,
                                  Type::get<typename std::decay<Ret>::type>(),
                                  BuildArgTypes<Args...>());
                ov.invoke = [func](void *instance,
                                   const std::vector<Value> &args,
                                   Value &out,
                                   std::string &err) -> bool
                {
                    return InvokeConstMember<Ret, Args...>(static_cast<T *>(instance), func, args, out, err);
                };

                MethodInfo &group = info_->methods[name];
                group.name = name;
                group.overloads.push_back(ov);
                return *this;
            }

            // 注册构造函数签名。
            // 构造成功后统一返回 T*，由 destroy 负责释放。
            template <typename... Args>
            class_builder &constructor()
            {
                ConstructorInfo ctor;
                ctor.arg_types = BuildArgTypes<Args...>();
                ctor.invoke = [](const std::vector<Value> &args, Value &out, std::string &err) -> bool
                {
                    if (args.size() != sizeof...(Args))
                    {
                        err = "constructor invoke failed: arg count mismatch";
                        return false;
                    }

                    return InvokeConstructor<Args...>(args, out, err, make_index_sequence<sizeof...(Args)>());
                };

                info_->constructors.push_back(ctor);
                return *this;
            }

            std::shared_ptr<TypeInfo> get()
            {
                return info_;
            }

        private:
            // 把模板参数包转成运行时类型列表，供匹配和错误信息使用。
            template <typename... Args>
            static std::vector<Type> BuildArgTypes()
            {
                std::vector<Type> out;
                out.reserve(sizeof...(Args));
                int dummy[] = {0, (out.push_back(Type::get<typename std::decay<Args>::type>()), 0)...};
                (void)dummy;
                return out;
            }

            // 把运行时参数逐个取出并调用真实成员函数。
            // 这里使用 index_sequence 展开参数包，避免手写递归。
            template <typename Ret, typename... Args, size_t... Is>
            static typename std::enable_if<!std::is_void<Ret>::value, bool>::type InvokeMemberImpl(
                T *instance,
                Ret (T::*func)(Args...),
                const std::vector<Value> &args,
                Value &out,
                std::string &, index_sequence<Is...>)
            {
                Ret result = (instance->*func)(args[Is].get_value<typename std::decay<Args>::type>()...);
                out = Value(result);
                return true;
            }

            // void 返回值走另一条路径：调用后清空 out。
            template <typename Ret, typename... Args, size_t... Is>
            static typename std::enable_if<std::is_void<Ret>::value, bool>::type InvokeMemberImpl(
                T *instance,
                Ret (T::*func)(Args...),
                const std::vector<Value> &args,
                Value &out,
                std::string &, index_sequence<Is...>)
            {
                (instance->*func)(args[Is].get_value<typename std::decay<Args>::type>()...);
                out.clear();
                return true;
            }

            // 成员函数调用外壳：先检查对象和参数数量，再交给展开逻辑。
            template <typename Ret, typename... Args>
            static bool InvokeMember(T *instance,
                                     Ret (T::*func)(Args...),
                                     const std::vector<Value> &args,
                                     Value &out,
                                     std::string &err)
            {
                if (!instance)
                {
                    err = "method invoke failed: instance is null";
                    return false;
                }

                if (args.size() != sizeof...(Args))
                {
                    err = "method invoke failed: arg count mismatch";
                    return false;
                }

                return InvokeMemberImpl<Ret, Args...>(instance, func, args, out, err, make_index_sequence<sizeof...(Args)>());
            }

            // const 成员函数的展开逻辑，和普通成员函数基本一致。
            template <typename Ret, typename... Args, size_t... Is>
            static typename std::enable_if<!std::is_void<Ret>::value, bool>::type InvokeConstMemberImpl(
                T *instance,
                Ret (T::*func)(Args...) const,
                const std::vector<Value> &args,
                Value &out,
                std::string &, index_sequence<Is...>)
            {
                Ret result = (instance->*func)(args[Is].get_value<typename std::decay<Args>::type>()...);
                out = Value(result);
                return true;
            }

            // void const 成员函数路径。
            template <typename Ret, typename... Args, size_t... Is>
            static typename std::enable_if<std::is_void<Ret>::value, bool>::type InvokeConstMemberImpl(
                T *instance,
                Ret (T::*func)(Args...) const,
                const std::vector<Value> &args,
                Value &out,
                std::string &, index_sequence<Is...>)
            {
                (instance->*func)(args[Is].get_value<typename std::decay<Args>::type>()...);
                out.clear();
                return true;
            }

            // const 成员函数调用外壳。
            template <typename Ret, typename... Args>
            static bool InvokeConstMember(T *instance,
                                          Ret (T::*func)(Args...) const,
                                          const std::vector<Value> &args,
                                          Value &out,
                                          std::string &err)
            {
                if (!instance)
                {
                    err = "method invoke failed: instance is null";
                    return false;
                }

                if (args.size() != sizeof...(Args))
                {
                    err = "method invoke failed: arg count mismatch";
                    return false;
                }

                return InvokeConstMemberImpl<Ret, Args...>(instance, func, args, out, err, make_index_sequence<sizeof...(Args)>());
            }

            // 构造函数展开调用：new T(arg0, arg1, ...)。
            template <typename... Args, size_t... Is>
            static bool InvokeConstructor(const std::vector<Value> &args,
                                          Value &out,
                                          std::string &, index_sequence<Is...>)
            {
                T *obj = new T(args[Is].get_value<typename std::decay<Args>::type>()...);
                out = Value(obj);
                return true;
            }

            std::shared_ptr<TypeInfo> info_;
        };

        template <typename T>
        class_builder<T> class_(const std::string &name)
        {
            return class_builder<T>(Registry::instance().get_or_create<T>(name));
        }

    } // namespace reflection
} // namespace eular

#endif // __EULAR_REFLECTION_MINI_REFLECT_H__
