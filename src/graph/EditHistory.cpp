#include "graph/EditHistory.h"

#include "ui/NodeRegistry.h"

namespace NodeSynth
{
	void FEditHistory::Push(FEditCommand Cmd)
	{
		if (bInComposite)
		{
			// Accumulate into the current composite entry — don't yet drop the
			// redo stack; that happens only when the composite is finalised.
			CurrentComposite.push_back(std::move(Cmd));
			return;
		}

		FUndoEntry Entry;
		Entry.push_back(std::move(Cmd));
		UndoStack.push_back(std::move(Entry));
		if (UndoStack.size() > MaxEntries)
		{
			UndoStack.erase(UndoStack.begin());
		}
		// New user action drops the redo stack.
		RedoStack.clear();
	}

	void FEditHistory::BeginComposite()
	{
		// Nested composites flatten into the outer one — simpler than tracking
		// a depth counter and gives the user-expected "one batch = one undo".
		bInComposite = true;
	}

	void FEditHistory::EndComposite()
	{
		if (!bInComposite)
		{
			return;
		}
		bInComposite = false;
		if (CurrentComposite.empty())
		{
			return;  // empty composite is a no-op
		}
		UndoStack.push_back(std::move(CurrentComposite));
		CurrentComposite.clear();
		if (UndoStack.size() > MaxEntries)
		{
			UndoStack.erase(UndoStack.begin());
		}
		RedoStack.clear();
	}

	bool FEditHistory::Undo(FGraphModel& Model)
	{
		if (UndoStack.empty()) { return false; }
		FUndoEntry Entry = std::move(UndoStack.back());
		UndoStack.pop_back();
		ApplyEntry(Entry, Model, /*bForward*/ false);
		RedoStack.push_back(std::move(Entry));
		return true;
	}

	bool FEditHistory::Redo(FGraphModel& Model)
	{
		if (RedoStack.empty()) { return false; }
		FUndoEntry Entry = std::move(RedoStack.back());
		RedoStack.pop_back();
		ApplyEntry(Entry, Model, /*bForward*/ true);
		UndoStack.push_back(std::move(Entry));
		return true;
	}

	void FEditHistory::ApplyEntry(const FUndoEntry& Entry, FGraphModel& Model, bool bForward)
	{
		// Forward: replay in original order. Undo: reverse order so dependent
		// commands undo before the things they depended on (e.g. an AddLink
		// that came after an AddNode is undone first; the AddNode undo then
		// finds no incident links).
		if (bForward)
		{
			for (const FEditCommand& Cmd : Entry)
			{
				Apply(Cmd, Model, true);
			}
		}
		else
		{
			for (auto It = Entry.rbegin(); It != Entry.rend(); ++It)
			{
				Apply(*It, Model, false);
			}
		}
	}

	void FEditHistory::Apply(const FEditCommand& Cmd, FGraphModel& Model, bool bForward)
	{
		// Suppress recording while replaying so we don't recurse.
		Model.SetRecordHistory(false);

		switch (Cmd.Type)
		{
			case EEditCommand::AddNode:
			{
				if (bForward)
				{
					Model.AddNodeWithId(Cmd.NodeId, MakeNodeByTypeName(Cmd.NodeType), Cmd.PosX, Cmd.PosY);
				}
				else
				{
					Model.RemoveNode(Cmd.NodeId);
				}
				break;
			}
			case EEditCommand::RemoveNode:
			{
				if (bForward)
				{
					Model.RemoveNode(Cmd.NodeId);
				}
				else
				{
					// Restore the node, its params, its per-voice flag, and its incident links.
					auto Node = MakeNodeByTypeName(Cmd.NodeType);
					Model.AddNodeWithId(Cmd.NodeId, Node, Cmd.PosX, Cmd.PosY);
					if (Node)
					{
						const auto Infos = Node->GetParamInfos();
						for (const auto& [Name, Value] : Cmd.Params)
						{
							for (uint32_t I = 0; I < Infos.size(); ++I)
							{
								if (Infos[I].Name == Name)
								{
									Node->SetParamValue(I, Value);
									break;
								}
							}
						}
					}
					if (Cmd.bPerVoice)
					{
						Model.SetNodePerVoice(Cmd.NodeId, true);
					}
					for (const auto& L : Cmd.IncidentLinks)
					{
						Model.AddLinkWithId(L.Id, L.FromNode, L.FromPort, L.ToNode, L.ToPort);
					}
				}
				break;
			}
			case EEditCommand::AddLink:
			{
				if (bForward)
				{
					Model.AddLinkWithId(Cmd.LinkId, Cmd.FromNode, Cmd.FromPort, Cmd.ToNode, Cmd.ToPort);
				}
				else
				{
					Model.RemoveLink(Cmd.LinkId);
				}
				break;
			}
			case EEditCommand::RemoveLink:
			{
				if (bForward)
				{
					Model.RemoveLink(Cmd.LinkId);
				}
				else
				{
					Model.AddLinkWithId(Cmd.LinkId, Cmd.FromNode, Cmd.FromPort, Cmd.ToNode, Cmd.ToPort);
				}
				break;
			}
			case EEditCommand::SetParam:
			{
				if (FNodeRecord* Rec = Model.FindNode(Cmd.NodeId); Rec && Rec->Node)
				{
					Rec->Node->SetParamValue(Cmd.ParamIndex, bForward ? Cmd.NewValue : Cmd.OldValue);
				}
				break;
			}
			case EEditCommand::SetNodePerVoice:
			{
				Model.SetNodePerVoice(Cmd.NodeId, bForward ? Cmd.NewPerVoice : Cmd.OldPerVoice);
				break;
			}
		}

		Model.SetRecordHistory(true);
	}
}
