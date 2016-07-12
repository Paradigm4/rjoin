/*
**
* BEGIN_COPYRIGHT
*
* Copyright (C) 2008-2016 SciDB, Inc.
* All Rights Reserved.
*
* rjoin is a plugin for SciDB, an Open Source Array DBMS maintained
* by Paradigm4. See http://www.paradigm4.com/
*
* rjoin is free software: you can redistribute it and/or modify
* it under the terms of the AFFERO GNU General Public License as published by
* the Free Software Foundation.
*
* rjoin is distributed "AS-IS" AND WITHOUT ANY WARRANTY OF ANY KIND,
* INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,
* NON-INFRINGEMENT, OR FITNESS FOR A PARTICULAR PURPOSE. See
* the AFFERO GNU General Public License for the complete license terms.
*
* You should have received a copy of the AFFERO GNU General Public License
* along with rjoin.  If not, see <http://www.gnu.org/licenses/agpl-3.0.html>
*
* END_COPYRIGHT
*/

#include <query/Operator.h>
#include "JoinHashTable.h"


namespace scidb
{

using namespace std;
using namespace rjoin;

namespace rjoin
{

class MemArrayAppender : public boost::noncopyable
{
private:
    shared_ptr<Array> _output;
    InstanceID const _myInstanceId;
    size_t const _numAttributes;
    size_t const _leftTupleSize;
    size_t const _numKeys;
    size_t const _chunkSize;
    shared_ptr<Query> _query;
    Settings const& _settings;
    Coordinates _outputPosition;
    vector<shared_ptr<ArrayIterator> >_arrayIterators;
    vector<shared_ptr<ChunkIterator> >_chunkIterators;

public:
    MemArrayAppender(Settings const& settings, shared_ptr<Query> const& query, string const name = ""):
        _output(make_shared<MemArray>(settings.getOutputSchema(query, name), query)),
        _myInstanceId(query->getInstanceID()),
        _numAttributes(settings.getNumOutputAttrs()),
        _leftTupleSize(settings.getLeftTupleSize()),
        _numKeys(settings.getNumKeys()),
        _chunkSize(settings.getChunkSize()),
        _query(query),
        _settings(settings),
        _outputPosition(2 , 0),
        _arrayIterators(_numAttributes, NULL),
        _chunkIterators(_numAttributes, NULL)
    {
        _outputPosition[0] = _myInstanceId;
        _outputPosition[1] = 0;
        for(size_t i =0; i<_numAttributes; ++i)
        {
            _arrayIterators[i] = _output->getIterator(i);
        }
    }

public:
    void writeTuple(vector<Value const*> const& left, Value const* right)
    {
        if( _outputPosition[1] % _chunkSize == 0)
        {
            for(size_t i=0; i<_numAttributes; ++i)
            {
                if(_chunkIterators[i].get())
                {
                    _chunkIterators[i]->flush();
                }
                _chunkIterators[i] = _arrayIterators[i]->newChunk(_outputPosition).getIterator(_query, i == 0 ?
                                                                                ChunkIterator::SEQUENTIAL_WRITE :
                                                                                ChunkIterator::SEQUENTIAL_WRITE | ChunkIterator::NO_EMPTY_CHECK);
            }
        }
        for(size_t i=0; i<_numAttributes; ++i)
        {
            _chunkIterators[i]->setPosition(_outputPosition);
            if(i<_leftTupleSize)
            {
                _chunkIterators[i]->writeItem(*(left[i]));
            }
            else
            {
                Value const* v = right + _numKeys + i - _leftTupleSize;
                _chunkIterators[i]->writeItem(*v);
            }
        }
        ++_outputPosition[1];
    }

    void writeTuple(Value const* left, vector<Value const*> const& right)
    {
        if( _outputPosition[1] % _chunkSize == 0)
        {
            for(size_t i=0; i<_numAttributes; ++i)
            {
                if(_chunkIterators[i].get())
                {
                    _chunkIterators[i]->flush();
                }
                _chunkIterators[i] = _arrayIterators[i]->newChunk(_outputPosition).getIterator(_query, i == 0 ?
                                                                                ChunkIterator::SEQUENTIAL_WRITE :
                                                                                ChunkIterator::SEQUENTIAL_WRITE | ChunkIterator::NO_EMPTY_CHECK);
            }
        }
        for(size_t i=0; i<_numAttributes; ++i)
        {
            _chunkIterators[i]->setPosition(_outputPosition);
            if(i<_leftTupleSize)
            {
                _chunkIterators[i]->writeItem(left[i]);
            }
            else
            {
                _chunkIterators[i]->writeItem(*(right[i - _leftTupleSize + _numKeys ]));
            }
        }
        ++_outputPosition[1];
    }

    shared_ptr<Array> finalize()
    {
        for(size_t i =0; i<_numAttributes; ++i)
        {
            if(_chunkIterators[i].get())
            {
                _chunkIterators[i]->flush();
            }
            _chunkIterators[i].reset();
            _arrayIterators[i].reset();
        }
        shared_ptr<Array> result = _output;
        _output.reset();
        return result;
    }
};

} //namespace rjoin


class PhysicalRJoin : public PhysicalOperator
{
public:
    PhysicalRJoin(string const& logicalName,
                             string const& physicalName,
                             Parameters const& parameters,
                             ArrayDesc const& schema):
         PhysicalOperator(logicalName, physicalName, parameters, schema)
    {}

    virtual bool changesDistribution(std::vector<ArrayDesc> const&) const
    {
        return true;
    }

    virtual RedistributeContext getOutputDistribution(
               std::vector<RedistributeContext> const& inputDistributions,
               std::vector< ArrayDesc> const& inputSchemas) const
    {
        return RedistributeContext(createDistribution(psUndefined), _schema.getResidency() );
    }

    //For hash join purposes, the handedness refers to which array is copied into a hash table and redistributed
    enum Handedness
    {
        LEFT,
        RIGHT
    };

    template <Handedness which>
    void readIntoTable(shared_ptr<Array> & array, JoinHashTable& table, Settings const& settings)
    {
        size_t const nAttrs = (which == LEFT ?  settings.getNumLeftAttrs() : settings.getNumRightAttrs());
        size_t const nDims  = (which == LEFT ?  settings.getNumLeftDims()  : settings.getNumRightDims());
        vector<Value const*> tuple ( which == LEFT ? settings.getLeftTupleSize() : settings.getRightTupleSize(), NULL);
        vector<shared_ptr<ConstArrayIterator> > aiters(nAttrs);
        vector<shared_ptr<ConstChunkIterator> > citers(nAttrs);
        vector<Value> dimVal(nDims);
        for(size_t i=0; i<nAttrs; ++i)
        {
            aiters[i] = array->getConstIterator(i);
        }
        while(!aiters[0]->end())
        {
            for(size_t i=0; i<nAttrs; ++i)
            {
                citers[i] = aiters[i]->getChunk().getConstIterator();
            }
            while(!citers[0]->end())
            {
                for(size_t i=0; i<nAttrs; ++i)
                {
                    ssize_t idx = which == LEFT ? settings.mapLeftToTuple(i) : settings.mapRightToTuple(i);
                    if (idx >= 0)
                    {
                        Value const& v = citers[i]->getItem();
                        tuple[ idx ] = &v;
                    }
                }
                Coordinates const& pos = citers[0]->getPosition();
                for(size_t i = 0; i<nDims; ++i)
                {
                    ssize_t idx = which == LEFT ? settings.mapLeftToTuple(i + nAttrs) : settings.mapRightToTuple(i + nAttrs);
                    if(idx >= 0)
                    {
                        dimVal[i].setInt64(pos[i]);
                        tuple [ idx ] = &dimVal[i];
                    }
                }
                table.insert(tuple);
                for(size_t i=0; i<nAttrs; ++i)
                {
                    ++(*citers[i]);
                }
            }
            for(size_t i=0; i<nAttrs; ++i)
            {
                ++(*aiters[i]);
            }
        }
    }

    template <Handedness which>
    shared_ptr<Array> arrayToTableJoin(shared_ptr<Array>& array, JoinHashTable& table, shared_ptr<Query>& query, Settings const& settings)
    {
        //handedness LEFT means the LEFT array is in table so this reads in reverse
        size_t const nAttrs = (which == LEFT ?  settings.getNumRightAttrs() : settings.getNumLeftAttrs());
        size_t const nDims  = (which == LEFT ?  settings.getNumRightDims()  : settings.getNumLeftDims());
        vector<Value const*> tuple ( which == LEFT ? settings.getRightTupleSize() : settings.getLeftTupleSize(), NULL);
        vector<shared_ptr<ConstArrayIterator> > aiters(nAttrs, NULL);
        vector<shared_ptr<ConstChunkIterator> > citers(nAttrs, NULL);
        vector<Value> dimVal(nDims);
        JoinHashTable::const_iterator iter = table.getIterator();
        MemArrayAppender result (settings, query);
        for(size_t i=0; i<nAttrs; ++i)
        {
            aiters[i] = array->getConstIterator(i);
        }
        while(!aiters[0]->end())
        {
            for(size_t i=0; i<nAttrs; ++i)
            {
                citers[i] = aiters[i]->getChunk().getConstIterator();
            }
            while(!citers[0]->end())
            {
                for(size_t i=0; i<nAttrs; ++i)
                {
                    Value const& v = citers[i]->getItem();
                    ssize_t idx = which == LEFT ? settings.mapRightToTuple(i) : settings.mapLeftToTuple(i);
                    if (idx >= 0)
                    {
                       Value const& v = citers[i]->getItem();
                       tuple[ idx ] = &v;
                    }
                }
                Coordinates const& pos = citers[0]->getPosition();
                for(size_t i =0; i<nDims; ++i)
                {
                    ssize_t idx = which == LEFT ? settings.mapRightToTuple(i + nAttrs) : settings.mapLeftToTuple(i + nAttrs);
                    if(idx >= 0)
                    {
                        dimVal[i].setInt64(pos[i]);
                        tuple [ idx ] = &dimVal[i];
                    }
                }
                iter.find(tuple);
                while(!iter.end() && iter.atKeys(tuple))
                {
                    Value const* tablePiece = iter.getTuple();
                    if(which == LEFT)
                    {
                        result.writeTuple(tablePiece, tuple);
                    }
                    else
                    {
                        result.writeTuple(tuple, tablePiece);
                    }
                    iter.next();
                }
                for(size_t i=0; i<nAttrs; ++i)
                {
                    ++(*citers[i]);
                }
            }
            for(size_t i=0; i<nAttrs; ++i)
            {
                ++(*aiters[i]);
            }
        }
        return result.finalize();
    }

    template <Handedness which>
    shared_ptr<Array> replicationHashJoin(vector< shared_ptr< Array> >& inputArrays, shared_ptr<Query> query, Settings const& settings)
    {
        shared_ptr<Array> redistributed = (which == LEFT ? inputArrays[0] : inputArrays[1]);
        redistributed = redistributeToRandomAccess(redistributed, createDistribution(psReplication), ArrayResPtr(), query);
        ArenaPtr operatorArena = this->getArena();
        ArenaPtr hashArena(newArena(Options("").resetting(true).threading(false).pagesize(8 * 1024 * 1204).parent(operatorArena)));
        JoinHashTable table(settings, hashArena, which == LEFT ? settings.getLeftTupleSize() : settings.getRightTupleSize());
        readIntoTable<which> (redistributed, table, settings);
        return arrayToTableJoin<which>( which == LEFT ? inputArrays[1]: inputArrays[0], table, query, settings);
    }

//    size_t findSizeLowerBound(shared_ptr<Array> &input, shared_ptr<Query>& query, size_t const sizeLimit, size_t const stringSize)
//    {
//        //input->getSupportedAccess()
//
//    }

    shared_ptr< Array> execute(vector< shared_ptr< Array> >& inputArrays, shared_ptr<Query> query)
    {
        vector<ArrayDesc const*> inputSchemas(2);
        inputSchemas[0] = &inputArrays[0]->getArrayDesc();
        inputSchemas[1] = &inputArrays[1]->getArrayDesc();
        Settings settings(inputSchemas, _parameters, false, query);
        if(inputArrays[0]->getSupportedAccess() == Array::SINGLE_PASS)
        {
            LOG4CXX_DEBUG(logger, "RJN moving left to random access");
            inputArrays[0] = ensureRandomAccess(inputArrays[0], query);
        }
        if(inputArrays[1]->getSupportedAccess() == Array::SINGLE_PASS)
        {
            LOG4CXX_DEBUG(logger, "RJN moving right to random access");
            inputArrays[1] = ensureRandomAccess(inputArrays[1], query);
        }
        if(settings.getAlgorithm() == Settings::LEFT_TO_RIGHT)
        {
            return replicationHashJoin<LEFT>(inputArrays, query, settings);
        }
        else
        {
            return replicationHashJoin<RIGHT>(inputArrays, query, settings);
        }
    }
};

REGISTER_PHYSICAL_OPERATOR_FACTORY(PhysicalRJoin, "rjoin", "physical_rjoin");
} //namespace scidb
