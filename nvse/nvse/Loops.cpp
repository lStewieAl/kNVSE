#include "Loops.h"
#include "ThreadLocal.h"
#include "ArrayVar.h"
#include "StringVar.h"
#include "ScriptUtils.h"
#include "GameAPI.h"
#include "GameObjects.h"
#include "GameRTTI.h"

LoopManager* LoopManager::GetSingleton()
{
	ThreadLocalData& localData = ThreadLocalData::Get();
	if (!localData.loopManager) {
		localData.loopManager = new LoopManager();
	}

	return localData.loopManager;
}

ArrayIterLoop::ArrayIterLoop(const ForEachContext* context, UInt8 modIndex)
{
	m_srcID = context->sourceID;
	m_iterID = context->iteratorID;
	m_iterVar = context->var;

	// clear the iterator var before initializing it
	g_ArrayMap.RemoveReference(&m_iterVar->data, modIndex);
	g_ArrayMap.AddReference(&m_iterVar->data, context->iteratorID, 0xFF);

	ArrayVar *arr = g_ArrayMap.Get(m_srcID);
	if (arr)
	{
		const ArrayKey *key;
		ArrayElement *elem;
		if (arr->GetFirstElement(&elem, &key))
		{
			m_curKey = *key;
			UpdateIterator(elem);		// initialize iterator to first element in array
		}
	}
}

void ArrayIterLoop::UpdateIterator(const ArrayElement* elem)
{
	ArrayVar *arr = g_ArrayMap.Get(m_iterID);
	if (!arr) return;

	// iter["key"] = element key
	ArrayElement *newElem = arr->Get("key", true);
	if (newElem)
	{
		if (m_curKey.KeyType() == kDataType_String)
			newElem->SetString(m_curKey.key.str);
		else newElem->SetNumber(m_curKey.key.num);
	}
	// iter["value"] = element data
	newElem = arr->Get("value", true);
	if (newElem) newElem->Set(elem);
}

bool ArrayIterLoop::Update(COMMAND_ARGS)
{
	ArrayVar *arr = g_ArrayMap.Get(m_srcID);
	if (arr)
	{
		ArrayElement *elem;
		const ArrayKey *key;
		if (arr->GetNextElement(&m_curKey, &elem, &key))
		{
			m_curKey = *key;
			UpdateIterator(elem);	
			return true;
		}
	}
	return false;
}

ArrayIterLoop::~ArrayIterLoop()
{
	//g_ArrayMap.RemoveReference(&m_iterID, 0xFF);
	g_ArrayMap.RemoveReference(&m_iterVar->data, 0xFF);
}

StringIterLoop::StringIterLoop(const ForEachContext* context)
{
	StringVar* srcVar = g_StringMap.Get(context->sourceID);
	StringVar* iterVar = g_StringMap.Get(context->iteratorID);
	if (srcVar && iterVar)
	{
		m_src = srcVar->String();
		m_curIndex = 0;
		m_iterID = context->iteratorID;
		if (m_src.length())
			iterVar->Set(m_src.substr(0, 1).c_str());
	}
}

bool StringIterLoop::Update(COMMAND_ARGS)
{
	StringVar* iterVar = g_StringMap.Get(m_iterID);
	if (iterVar)
	{
		m_curIndex++;
		if (m_curIndex < m_src.length())
		{
			iterVar->Set(m_src.substr(m_curIndex, 1).c_str());
			return true;
		}
	}

	return false;
}

ContainerIterLoop::ContainerIterLoop(const ForEachContext* context)
{
	TESObjectREFR* contRef = DYNAMIC_CAST((TESForm*)context->sourceID, TESForm, TESObjectREFR);
	m_refVar = context->var;
	m_iterIndex = 0;
	m_invRef = CreateInventoryRef(contRef, IRefData(NULL, NULL, NULL), false);	

	// first: figure out what items exist by default
	TESContainer* cont = DYNAMIC_CAST(contRef->baseForm, TESForm, TESContainer);
	if (cont)
	{
		UnorderedMap<TESForm*, SInt32> baseObjectCounts;
		for (TESContainer::FormCountList::Iterator cur = cont->formCountList.Begin(); !cur.End(); ++cur)
		{
			if (cur.Get() && cur.Get()->form && cur.Get()->form->typeID != kFormType_LeveledItem)
			{
				//DEBUG_PRINT("Base container has %d %s", cur.Get()->count, GetFullName(cur.Get()->form));
				baseObjectCounts[cur.Get()->form] = cur.Get()->count;
			}
		}
	
		// now populate the vec
		ExtraContainerChanges* xChanges = (ExtraContainerChanges*)contRef->extraDataList.GetByType(kExtraData_ContainerChanges);
		if (xChanges && xChanges->data)
		{
			for (ExtraContainerChanges::EntryDataList::Iterator entry = xChanges->data->objList->Begin(); !entry.End(); ++entry)
			{
				if (entry.Get())
				{
					TESForm* baseObj = entry.Get()->type;

					SInt32 countDelta = entry.Get()->countDelta;
					SInt32 actualCount = countDelta;
					SInt32 *isInBaseContainer = baseObjectCounts.GetPtr(baseObj);
					SInt32 xCount;
					if (isInBaseContainer)
					{
						*isInBaseContainer += countDelta;
						actualCount = *isInBaseContainer;
					}

					if (entry.Get()->extendData)
					{
						UInt32 total = 0;
						for (ExtraContainerChanges::ExtendDataList::Iterator extend = entry.Get()->extendData->Begin(); !extend.End(); ++extend)
						{
							if (total >= actualCount)
								break;

							xCount = GetCountForExtraDataList(extend.Get());
							total += xCount;
							m_elements.Append(CreateTempEntry(baseObj, xCount, extend.Get()));
						}

						SInt32 remainder = isInBaseContainer ? *isInBaseContainer : countDelta;
						remainder -= total;
						if (remainder > 0)
							m_elements.Append(CreateTempEntry(baseObj, remainder, NULL));
					}
					else
					{
						SInt32 actualCount = countDelta;
						if (isInBaseContainer)
							actualCount += *isInBaseContainer;

						if (actualCount > 0)
							m_elements.Append(CreateTempEntry(baseObj, actualCount, NULL));
					}

					if (isInBaseContainer)
						baseObjectCounts.Erase(baseObj);
				}
				else {
					DEBUG_PRINT("Warning: encountered NULL ExtraContainerChanges::Entry::Data pointer in ContainerIterLoop constructor.");
				}
			}
		}

		// now add entries for objects in base but without associated ExtraContainerChanges
		// these extra entries will be removed when we're done with the loop
		if (!baseObjectCounts.Empty())
		{
			for (auto iter = baseObjectCounts.Begin(); !iter.End(); ++iter)
			{
				if (*iter > 0)
					m_elements.Append(CreateTempEntry(iter.Key(), *iter, NULL));
			}
		}
	}

	// initialize the iterator
	SetIterator();
}

bool ContainerIterLoop::UnsetIterator()
{
	return m_invRef->WriteRefDataToContainer();
}

bool ContainerIterLoop::SetIterator()
{
	TESObjectREFR* refr = m_invRef->GetRef();
	if (m_iterIndex < m_elements.Size() && refr)
	{
		ExtraContainerChanges::EntryData *entry = m_elements[m_iterIndex];
		ExtraDataList *xData = entry->extendData ? entry->extendData->GetFirstItem() : NULL;
		m_invRef->SetData(IRefData(entry->type, entry, xData));
		*((UInt64*)&m_refVar->data) = refr->refID;
		return true;
	}
	else
	{
		// loop ends, ref will shortly be invalid so zero out the var
		m_refVar->data = 0;
		m_invRef->SetData(IRefData());
		return false;
	}
}

bool ContainerIterLoop::Update(COMMAND_ARGS)
{
	UnsetIterator();
	m_iterIndex++;
	return SetIterator();
}

ContainerIterLoop::~ContainerIterLoop()
{
	for (auto iter = m_elements.Begin(); !iter.End(); ++iter)
	{
		if (iter->extendData)
			GameHeapFree(iter->extendData);
		GameHeapFree(*iter);
	}
	m_invRef->Release();
	m_refVar->data = 0;
}

void LoopManager::Add(Loop* loop, ScriptRunner* state, UInt32 startOffset, UInt32 endOffset, COMMAND_ARGS)
{
	// save the instruction offsets
	LoopInfo loopInfo;
	loopInfo.loop = loop;
	loopInfo.endIP = endOffset;

	// save the stack
	SavedIPInfo* savedInfo = &loopInfo.ipInfo;
	savedInfo->ip = startOffset;
	savedInfo->stackDepth = state->ifStackDepth;
	memcpy(savedInfo->stack, state->ifStack, (savedInfo->stackDepth + 1) * sizeof(UInt32));

	m_loops.push(loopInfo);
}

void LoopManager::RestoreStack(ScriptRunner* state, SavedIPInfo* info)
{
	state->ifStackDepth = info->stackDepth;
	memcpy(state->ifStack, info->stack, (info->stackDepth + 1) * sizeof(UInt32));
}

bool LoopManager::Break(ScriptRunner* state, COMMAND_ARGS)
{
	if (!m_loops.size())
		return false;

	LoopInfo* loopInfo = &m_loops.top();

	RestoreStack(state, &loopInfo->ipInfo);

	ScriptRunner	* scriptRunner = GetScriptRunner(opcodeOffsetPtr);
	SInt32			* calculatedOpLength = GetCalculatedOpLength(opcodeOffsetPtr);

	// restore ip
	*calculatedOpLength += loopInfo->endIP - *opcodeOffsetPtr;

	delete loopInfo->loop;

	m_loops.pop();

	return true;
}

bool LoopManager::Continue(ScriptRunner* state, COMMAND_ARGS)
{
	if (!m_loops.size())
		return false;

	LoopInfo* loopInfo = &m_loops.top();

	if (!loopInfo->loop->Update(PASS_COMMAND_ARGS))
	{
		Break(state, PASS_COMMAND_ARGS);
		return true;
	}

	RestoreStack(state, &loopInfo->ipInfo);

	ScriptRunner	* scriptRunner = GetScriptRunner(opcodeOffsetPtr);
	SInt32			* calculatedOpLength = GetCalculatedOpLength(opcodeOffsetPtr);

	// restore ip
	*calculatedOpLength += loopInfo->ipInfo.ip - *opcodeOffsetPtr;

	return true;
}


bool WhileLoop::Update(COMMAND_ARGS)
{
	// save *opcodeOffsetPtr so we can calc IP to branch to after evaluating loop condition
	UInt32 originalOffset = *opcodeOffsetPtr;

	// update offset to point to loop condition, evaluate
	*opcodeOffsetPtr = m_exprOffset;
	ExpressionEvaluator eval(PASS_COMMAND_ARGS);
	bool bResult = eval.ExtractArgs();

	*opcodeOffsetPtr = originalOffset;

	if (bResult && eval.Arg(0))
	{
		bResult = eval.Arg(0)->GetBool();
	}

	return bResult;
}


static SmallObjectsAllocator::Allocator<WhileLoop, 8> g_whileLoopAllocator;

void* WhileLoop::operator new(size_t size)
{
	return g_whileLoopAllocator.Allocate();
}

void WhileLoop::operator delete(void* p)
{
	g_whileLoopAllocator.Free(p);
}
