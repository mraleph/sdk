// Copyright (c) 2020, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:kernel/ast.dart';
import 'package:kernel/core_types.dart';
import 'package:kernel/external_name.dart' show getExternalName;
import 'package:vm/transformations/type_flow/analysis.dart';
import 'package:vm/transformations/type_flow/native_code.dart';
import 'package:vm/transformations/type_flow/types.dart';

import '../../metadata/unboxing_info.dart';
import 'utils.dart';

class UnboxingInfoManager {
  final List<UnboxingInfoMetadata> _allUnboxingInfo = [];
  final Map<Member, int> _memberIds = {};
  final List<int> _partitionIds = [];
  final List<int> _partitionRank = [];
  final Set<Member> _mustBox = {};

  final TypeHierarchy _typeHierarchy;
  final CoreTypes _coreTypes;
  final NativeCodeOracle _nativeCodeOracle;
  bool _finishedGraph;

  UnboxingInfoManager(TypeFlowAnalysis typeFlowAnalysis)
      : _typeHierarchy = typeFlowAnalysis.hierarchyCache,
        _coreTypes = typeFlowAnalysis.environment.coreTypes,
        _nativeCodeOracle = typeFlowAnalysis.nativeCodeOracle,
        _finishedGraph = false;

  void registerMember(Member member) {
    member = _validMemberOrNull(member);
    if (member == null) return;
    _addMember(member);
  }

  UnboxingInfoMetadata getUnboxingInfoOfMember(Member member) {
    assert(_finishedGraph);
    member = _validMemberOrNull(member);
    if ((member == null || (!_memberIds.containsKey(member)))) {
      return null;
    }
    final partitionId = _memberIds[member];
    return _allUnboxingInfo[partitionId];
  }

  void linkWithSuperClasses(Member member) {
    member = _validMemberOrNull(member);
    if (member == null) return;
    _linkRecursive(member, member.enclosingClass);
  }

  void finishGraph() {
    assert(!_finishedGraph);
    final oldToNewId = {};
    final newUnboxingInfo = <UnboxingInfoMetadata>[];
    for (int i = 0; i < _allUnboxingInfo.length; i++) {
      if (_find(i) == i) {
        final newId = oldToNewId.length;
        oldToNewId[i] = newId;
        newUnboxingInfo.add(_allUnboxingInfo[i]);
      }
    }
    for (final key in _memberIds.keys) {
      final newId = oldToNewId[_partitionIds[_memberIds[key]]];
      assert(newId != null);
      _memberIds[key] = newId;
    }
    _allUnboxingInfo.clear();
    _allUnboxingInfo.insertAll(0, newUnboxingInfo);
    _memberIds.forEach((member, id) {
      if (_mustBox.contains(member)) {
        _allUnboxingInfo[id].returnInfo = UnboxingInfoMetadata.kBoxed;
        _allUnboxingInfo[id].unboxedArgsInfo.clear();
      }
    });
    _partitionIds.clear();
    _partitionRank.clear();
    _finishedGraph = true;
  }

  void applyToArg(Member member, int argPos, Type type) {
    assert(_finishedGraph);
    member = _validMemberOrNull(member);
    if (member == null) return;
    assert(_memberIds.containsKey(member));
    final partitionId = _memberIds[member];

    if (argPos < 0 ||
        _allUnboxingInfo[partitionId].unboxedArgsInfo.length <= argPos) {
      return;
    }

    final unboxingInfo = _allUnboxingInfo[partitionId];

    if (type is NullableType ||
        (!type.isSubtypeOf(_typeHierarchy, _coreTypes.intClass) &&
            !type.isSubtypeOf(_typeHierarchy, _coreTypes.doubleClass))) {
      unboxingInfo.unboxedArgsInfo[argPos] = UnboxingInfoMetadata.kBoxed;
    } else {
      final unboxingType = type.isSubtypeOf(_typeHierarchy, _coreTypes.intClass)
          ? UnboxingInfoMetadata.kUnboxedIntCandidate
          : UnboxingInfoMetadata.kUnboxedDoubleCandidate;
      unboxingInfo.unboxedArgsInfo[argPos] &= unboxingType;
    }
  }

  void applyToReturn(Member member, Type type) {
    assert(_finishedGraph);
    member = _validMemberOrNull(member);
    if (member == null) return;
    assert(_memberIds.containsKey(member));
    final partitionId = _memberIds[member];
    final unboxingInfo = _allUnboxingInfo[partitionId];

    if (type is NullableType ||
        (!type.isSubtypeOf(_typeHierarchy, _coreTypes.intClass) &&
            !type.isSubtypeOf(_typeHierarchy, _coreTypes.doubleClass))) {
      unboxingInfo.returnInfo = UnboxingInfoMetadata.kBoxed;
    } else {
      final unboxingType = type.isSubtypeOf(_typeHierarchy, _coreTypes.intClass)
          ? UnboxingInfoMetadata.kUnboxedIntCandidate
          : UnboxingInfoMetadata.kUnboxedDoubleCandidate;
      unboxingInfo.returnInfo &= unboxingType;
    }
  }

  void _linkRecursive(Member member, Class cls) {
    for (final superType in cls.supers) {
      final superClass = superType.classNode;
      bool linked = false;
      superClass.members.forEach((Member superMember) {
        if (member.isInstanceMember) {
          if (member.name == superMember.name) {
            _linkMembers(member, superMember);
            linked = true;
          }
        }
      });
      if (!linked) {
        _linkRecursive(member, superClass);
      }
    }
  }

  void _linkMembers(Member member1, Member member2) {
    member1 = _validMemberOrNull(member1);
    member2 = _validMemberOrNull(member2);
    if (member1 == null ||
        member2 == null ||
        _isConstructorOrStatic(member1) ||
        _isConstructorOrStatic(member2)) {
      return;
    }
    _union(_getMemberId(member1), _getMemberId(member2));
  }

  Member _validMemberOrNull(Member member) {
    if (member == null ||
        (member is! Procedure && member is! Constructor && member is! Field)) {
      return null;
    }
    return member;
  }

  bool _isConstructorOrStatic(Member member) {
    return ((member is Constructor) ||
        ((member is Procedure) && member.isStatic));
  }

  void _addMember(Member member) {
    assert(!_finishedGraph);

    if (_cannotUnbox(member)) {
      _mustBox.add(member);
    }

    final int memberId = _allUnboxingInfo.length;
    assert(memberId == _partitionIds.length);
    assert(_partitionIds.length == _partitionRank.length);
    final int argsLen = member is Field
        ? (member.hasSetter ? 1 : 0)
        : member.function.requiredParameterCount;
    _memberIds[member] = memberId;
    _allUnboxingInfo.add(UnboxingInfoMetadata(argsLen));
    _partitionIds.add(memberId);
    _partitionRank.add(1);
  }

  bool _cannotUnbox(Member member) {
    // Methods that do not need dynamic invocation forwarders can not have
    // unboxed parameters and return because dynamic calls always use boxed
    // values.
    // Similarly C->Dart calls (entrypoints) and Dart->C calls (natives) need to
    // have boxed parameters and return values.
    return (_isNative(member) ||
        _nativeCodeOracle.isMemberReferencedFromNativeCode(member) ||
        _isEnclosingClassSubtypeOfNum(member));
  }

  bool _isNative(Member member) {
    return (getExternalName(member) != null);
  }

  // TODO(dartbug.com/33549): Calls to these methods could be replaced by
  // CheckedSmiOpInstr, so in order to allow the parameters and return
  // value to be unboxed, the slow path for such instructions should be
  // updated to be consistent with the representations from the target interface.
  bool _isEnclosingClassSubtypeOfNum(Member member) {
    return (member.enclosingClass != null &&
        ConeType(_typeHierarchy.getTFClass(member.enclosingClass))
            .isSubtypeOf(_typeHierarchy, _coreTypes.numClass));
  }

  int _getMemberId(Member member) {
    return _memberIds[member];
  }

  int _find(int memberId) {
    assert(!_finishedGraph);
    if (memberId == _partitionIds[memberId]) {
      return memberId;
    }
    final partitionId = _find(_partitionIds[memberId]);
    _allUnboxingInfo[memberId] = null;
    _partitionIds[memberId] = partitionId;
    return partitionId;
  }

  void _union(int memberId1, int memberId2) {
    assert(!_finishedGraph);
    final partitionId1 = _find(memberId1);
    final partitionId2 = _find(memberId2);

    if (partitionId1 == partitionId2) {
      return;
    }

    int from, to;
    if (_partitionRank[partitionId1] < _partitionRank[partitionId2]) {
      from = partitionId1;
      to = partitionId2;
    } else {
      from = partitionId2;
      to = partitionId1;
    }
    final fromArgsInfo = _allUnboxingInfo[from].unboxedArgsInfo;
    final toArgsInfo = _allUnboxingInfo[to].unboxedArgsInfo;
    if (fromArgsInfo.length < toArgsInfo.length) {
      toArgsInfo.length = fromArgsInfo.length;
    }
    _allUnboxingInfo[from] = null;
    _partitionIds[from] = to;
    if (_partitionRank[from] == _partitionRank[to]) {
      _partitionRank[to]++;
    }
  }
}
