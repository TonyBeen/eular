 ### `thread.h`

> Linux线程类
> 用法：
>
> ​		1、继承ThreadBase，重写threadWorkFunction
>
> ​		2、使用Thread类创建线程，添加工作函数。
>
> ​			  类内提供了用户参数选项，使用setArg可以设置用户数据块，然内存释放不由Thread掌管。
>
> ​			  setArg在run之前调用，在run之后调用可能会导致段错误，如果用户在工作函数内对空指针做判断则不会

### `string8.h`

> 提供各种运算符如 = + += >= >...等
>
> 提供Format，append，toStdString函数

### `mutex.h`

> 采用 `pthread_mutex_t`
>
> 实现了AutoLock，模板类，作用：构造加锁，析构解锁



### `condition.h`

> 采用 `pthread_cond_t `
>
> 提供wait，single，broadcast，timedWait等函数
>
> wait参数为Alias::Mutex对象

