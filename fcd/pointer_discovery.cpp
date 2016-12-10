//
// pass_pointerdiscovery.cpp
// Copyright (C) 2015 Félix Cloutier.
// All Rights Reserved.
//
// This file is part of fcd.
// 
// fcd is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// fcd is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with fcd.  If not, see <http://www.gnu.org/licenses/>.
//

#include "metadata.h"
#include "pointer_discovery.h"

#include <llvm/IR/Constants.h>

using namespace llvm;
using namespace std;

namespace
{
	bool asMultiplication(Value& valueToTest, Value*& variable, int64_t& stride)
	{
		if (auto valueAsBinaryOperator = dyn_cast<BinaryOperator>(&valueToTest))
		{
			if (valueAsBinaryOperator->getOpcode() == BinaryOperator::Mul)
			{
				if (auto constantLeft = dyn_cast<ConstantInt>(valueAsBinaryOperator->getOperand(0)))
				{
					stride = static_cast<int64_t>(constantLeft->getLimitedValue());
					variable = valueAsBinaryOperator->getOperand(1);
					return true;
				}
				else if (auto constantRight = dyn_cast<ConstantInt>(valueAsBinaryOperator->getOperand(1)))
				{
					stride = static_cast<int64_t>(constantRight->getLimitedValue());
					variable = valueAsBinaryOperator->getOperand(0);
					return true;
				}
			}
			else if (valueAsBinaryOperator->getOpcode() == BinaryOperator::Shl)
			{
				if (auto constantShift = dyn_cast<ConstantInt>(valueAsBinaryOperator->getOperand(1)))
				{
					stride = 1ull << constantShift->getLimitedValue();
					variable = valueAsBinaryOperator->getOperand(0);
					return true;
				}
			}
		}
		return false;
	}
	
	void unifyObjectAddresses(ObjectAddress& left, ObjectAddress& right)
	{
		auto& rightSet = *right.unification;
		for (ObjectAddress* toMove : rightSet)
		{
			toMove->unification = left.unification;
			left.unification->insert(&right);
		}
		rightSet.clear();
	}
}

RootObjectAddress& RootObjectAddress::getRoot()
{
	return *this;
}

RelativeObjectAddress::RelativeObjectAddress(Type type, NOT_NULL(Value) value, UnificationSet unification, NOT_NULL(ObjectAddress) parent)
: ObjectAddress(type, value, unification), parent(parent)
{
}

RootObjectAddress& RelativeObjectAddress::getRoot()
{
	return parent->getRoot();
}

class FunctionPointerDiscovery
{
	PointerDiscovery& context;
	Function& function;
	unordered_map<Value*, ObjectAddress*> functionAddresses;
	
public:
	FunctionPointerDiscovery(PointerDiscovery& context, Function& function)
	: context(context), function(function)
	{
	}
	
	template<typename AddressType, typename... Arguments>
	AddressType& createAddress(llvm::Value& value, Arguments&&... args)
	{
		context.unificationSets.emplace_back();
		auto address = context.pool.allocate<AddressType>(&value, &context.unificationSets.back(), forward<Arguments>(args)...);
		context.unificationSets.back().insert(address);
		context.addressesInFunctions[&function].push_back(address);
		return *address;
	}
	
	RootObjectAddress& createRootAddress(llvm::Value& value)
	{
		auto& root = createAddress<RootObjectAddress>(value);
		auto result = context.roots.insert({&value, &root});
		assert(result.second); (void) result;
		return root;
	}
	
	ObjectAddress& handlePointerAdditionWithConstant(llvm::BinaryOperator& value, llvm::Value& left, llvm::ConstantInt& right, bool positive = true)
	{
		int64_t offset = static_cast<int64_t>(right.getLimitedValue());
		offset *= positive ? 1 : -1;
		ObjectAddress& base = createObjectAddress(left);
		return createAddress<ConstantOffsetObjectAddress>(value, &base, offset);
	}
	
	ObjectAddress& handlePointerAddition(llvm::BinaryOperator& addition, bool positive = true)
	{
		// Cases to handle:
		// 1- One side is variable and the other is constant;
		// 2- Both sides are variable, one of them is a multiplication (left shifts included);
		// 3- Both sides are variable, neither is a multiplication.
		if (auto constantLeft = dyn_cast<ConstantInt>(addition.getOperand(0)))
		{
			return handlePointerAdditionWithConstant(addition, *addition.getOperand(1), *constantLeft, positive);
		}
		else if (auto constantRight = dyn_cast<ConstantInt>(addition.getOperand(1)))
		{
			return handlePointerAdditionWithConstant(addition, *addition.getOperand(0), *constantRight, positive);
		}
		
		// Is one operand a multiplication then?
		int64_t stride;
		Value* index;
		ObjectAddress* base = nullptr;
		if (asMultiplication(*addition.getOperand(0), index, stride))
		{
			base = &createObjectAddress(*addition.getOperand(1));
		}
		else if (asMultiplication(*addition.getOperand(1), index, stride))
		{
			base = &createObjectAddress(*addition.getOperand(0));
		}
		else
		{
			// Both variables, pick more or less arbitrarily.
			// XXX: this could be made more reliable by checking if either side is part of a pointer chain.
			stride = 1;
			index = addition.getOperand(1);
			base = &createObjectAddress(*addition.getOperand(0));
		}
		
		stride *= positive ? 1 : -1;
		return createAddress<VariableOffsetObjectAddress>(addition, base, index, stride);
	}
	
	ObjectAddress& createObjectAddress(llvm::Value& value)
	{
		auto& resultAddress = functionAddresses[&value];
		if (resultAddress != nullptr)
		{
			return *resultAddress;
		}
		
		// Instructions that merely forward other object addresses
		if (auto castInst = dyn_cast<CastInst>(&value))
		{
			switch (castInst->getOpcode())
			{
				case Instruction::ZExt:
				case Instruction::IntToPtr:
				case Instruction::BitCast:
				case Instruction::AddrSpaceCast:
				{
					resultAddress = &createObjectAddress(*castInst->getOperand(0));
					return *resultAddress;
				}
				default: break;
			}
		}
		else if (auto select = dyn_cast<SelectInst>(&value))
		{
			resultAddress = &createRootAddress(*select);
			unifyObjectAddresses(*resultAddress, createObjectAddress(*select->getTrueValue()));
			unifyObjectAddresses(*resultAddress, createObjectAddress(*select->getFalseValue()));
			return *resultAddress;
		}
		else if (auto phi = dyn_cast<PHINode>(&value))
		{
			resultAddress = &createRootAddress(*phi);
			for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
			{
				unifyObjectAddresses(*resultAddress, createObjectAddress(*phi->getIncomingValue(i)));
			}
			return *resultAddress;
		}
		
		// Instructions that create relative addresses
		// XXX: this doesn't handle very well the case where things are mapped at address 0. Don't think that it's worth
		// implementing...
		else if (auto binaryOp = dyn_cast<BinaryOperator>(&value))
		{
			if (binaryOp->getOpcode() == BinaryOperator::Add)
			{
				resultAddress = &handlePointerAddition(*binaryOp);
				return *resultAddress;
			}
			else if (binaryOp->getOpcode() == BinaryOperator::Sub)
			{
				resultAddress = &handlePointerAddition(*binaryOp, false);
				return *resultAddress;
			}
		}
		
		resultAddress = &createRootAddress(value);
		return *resultAddress;
	}
};

void PointerDiscovery::analyzeFunction(Function& fn)
{
	auto addressesIter = addressesInFunctions.find(&fn);
	if (addressesIter != addressesInFunctions.end())
	{
		return;
	}
	
	// Ensure that the collection exists.
	(void) addressesInFunctions[&fn];
	
	if (md::isPrototype(fn))
	{
		// Functions with a known signature need to be handled at a later point.
		return;
	}
	
	FunctionPointerDiscovery functionContext(*this, fn);
	// Identify and process memory operations (loads, stores, calls).
	for (BasicBlock& bb : fn)
	{
		for (Instruction& inst : bb)
		{
			if (auto call = dyn_cast<CallInst>(&inst))
			{
				// Unify call parameters with arguments when they are pointers.
				if (auto callee = call->getCalledFunction())
				{
					analyzeFunction(*callee);
					unsigned argIndex = 0;
					for (Argument& parameter : callee->args())
					{
						auto iter = roots.find(&parameter);
						if (iter != roots.end())
						{
							ObjectAddress& argument = functionContext.createObjectAddress(*call->getArgOperand(argIndex));
							unifyObjectAddresses(*iter->second, argument);
						}
						++argIndex;
					}
				}
			}
			else if (auto load = dyn_cast<LoadInst>(&inst))
			{
				functionContext.createObjectAddress(*load->getPointerOperand());
			}
			else if (auto store = dyn_cast<StoreInst>(&inst))
			{
				functionContext.createObjectAddress(*store->getPointerOperand());
			}
		}
	}
}

void PointerDiscovery::analyzeModule(Module& module)
{
	pool.clear();
	unificationSets.clear();
	addressesInFunctions.clear();
	roots.clear();
	
	for (auto& function : module)
	{
		analyzeFunction(function);
	}
}
