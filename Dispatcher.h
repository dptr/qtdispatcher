#pragma once

#include <QObject>
#include <QMetaObject>
#include <QVariant>
#include <exception>
#include <functional>
#include <type_traits>

namespace ymarcov {
namespace qt {

class Dispatcher : public QObject
{
	Q_OBJECT

public:
	struct Task
	{
		template <typename Result>
		static Task Create(typename std::enable_if<!std::is_void<Result>::value &&
			!std::is_reference<Result>::value, std::function<Result()>>::type f)
		{
			return Task([=](void** result, std::exception_ptr& e)
			{
				try
				{
					*result = new std::decay<decltype(f())>::type(f());
				}
				catch (...)
				{
					e = std::current_exception();
				}
			});
		}

		template <typename Result>
		static Task Create(typename std::enable_if<!std::is_void<Result>::value &&
			std::is_reference<Result>::value, std::function<Result()>>::type f)
		{
			return Task([=](void** result, std::exception_ptr& e)
			{
				try
				{
					*result = const_cast<std::decay<decltype(f())>::type*>(&f());
				}
				catch (...)
				{
					e = std::current_exception();
				}
			});
		}

		template <typename Result>
		static Task Create(typename std::enable_if<std::is_void<Result>::value, std::function<void()>>::type f)
		{
			return Task([=](void**, std::exception_ptr& e)
			{
				try
				{
					f();
				}
				catch (...)
				{
					e = std::current_exception();
				}
			});
		}

		static Task CreateFireAndForget(std::function<void()> f)
		{
			return Task([=](void**, std::exception_ptr&)
			{
				f();
			});
		}

		Task() // QVariant needs a default constructor in case it fails to convert
		{}

		Task(std::function<void(void**, std::exception_ptr&)> shim)
			: _shim(std::move(shim))
			, _result(nullptr)
		{}

		void operator()()
		{
			_shim(&_result, _exception);
		}

		template <typename Result>
		typename std::enable_if<!std::is_void<Result>::value && std::is_reference<Result>::value, Result>::type Get()
		{
			if (_exception)
				std::rethrow_exception(_exception);

			return std::forward<Result>(*static_cast<std::decay<Result>::type*>(_result));
		}

		template <typename Result>
		typename std::enable_if<!std::is_void<Result>::value && !std::is_reference<Result>::value, Result>::type Get()
		{
			typedef std::decay<Result>::type RawType;

			std::unique_ptr<RawType> ptr(static_cast<RawType*>(_result));

			if (_exception)
				std::rethrow_exception(_exception);
			
			return std::move(*ptr);
		}

		template <typename Result>
		typename std::enable_if<std::is_void<Result>::value, void>::type Get()
		{
			if (_exception)
				std::rethrow_exception(_exception);
		}

		std::function<void(void**, std::exception_ptr&)> _shim;
		void* _result;
		std::exception_ptr _exception;
	};

	Dispatcher(QObject* parent = nullptr)
		: QObject(parent)
	{}

	// Runs a function and returns its possible value.
	// If the function throws an exception, it will be re-thrown here.
	template <typename Func>
	auto Invoke(Func f) const -> decltype(f())
	{
		typedef decltype(f()) Result;

		Task task = Task::Create<Result>(std::function<Result()>(std::forward<Func>(f)));
		QMetaObject::invokeMethod(
			const_cast<Dispatcher*>(this),
			"Dispatch",
			Qt::BlockingQueuedConnection,
			Q_ARG(QVariant, QVariant::fromValue(reinterpret_cast<intptr_t>(&task)))
		);
		return task.Get<Result>();
	}

	// Runs a function on a the dispatcher's thread.
	// Exceptions are caught by the dispatcher's message loop.
	template <typename Action>
	void FireAndForget(Action f) const
	{
		Task task = Task::CreateFireAndForget(std::function<void()>(std::forward<Action>(f)));
		QMetaObject::invokeMethod(
			const_cast<Dispatcher*>(this),
			"Dispatch",
			Qt::QueuedConnection,
			Q_ARG(QVariant, QVariant::fromValue(task))
		);
	}

private:
	Q_INVOKABLE void Dispatch(QVariant var)
	{
		if (var.canConvert<intptr_t>()) // blocking connection, pointer to callable on diff thread
			reinterpret_cast<Task*>(var.value<intptr_t>())->operator()();
		else if (var.canConvert<Task>()) // non blocking connection, callable was copied
			var.value<Task>()();
	}
};

} // namespace qt
} // namespace ymarcov

Q_DECLARE_METATYPE(ymarcov::qt::Dispatcher::Task);