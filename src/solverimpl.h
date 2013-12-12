/*------------------------------------------------------------------------------
| Copyright (c) 2013, Nucleic Development Team.
|
| Distributed under the terms of the Modified BSD License.
|
| The full license is in the file COPYING.txt, distributed with this software.
|-----------------------------------------------------------------------------*/
#pragma once
#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>
#include "constraint.h"
#include "errors.h"
#include "expression.h"
#include "maptype.h"
#include "relation.h"
#include "row.h"
#include "symbol.h"
#include "term.h"
#include "util.h"
#include "variable.h"


namespace kiwi
{

namespace impl
{

class SolverImpl
{
	struct CnTag
	{
		Symbol marker;
		Symbol error1;
		Symbol error2;
	};

	typedef MapType<Variable, Symbol>::Type VarMap;

	typedef MapType<Symbol, Row*>::Type RowMap;

	typedef MapType<Constraint, CnTag>::Type CnMap;

public:

	SolverImpl() : m_id_tick( 1 ), m_objective( new Row() ) {}

	~SolverImpl()
	{
		clearRows();
	}

	/* Add a constraint to the solver.

	If the constraint has already been added, this method is a no-op. If
	the strength of the constraint is required and the constraint cannot
	be satisfied, an UnsatisfiableConstraint exception is thrown.

	*/
	void addConstraint( const Constraint& constraint )
	{
		if( m_cns.find( constraint ) != m_cns.end() )
			return;

		CnTag tag;
		std::auto_ptr<Row> rowptr( createRow( constraint, tag ) );

		// If the constraint is unsatisfiable, the variables which were
		// added as a result of creating the row for the constraint are
		// not needed and should be removed. Code should not typically
		// add conflicting constraints and its likely that a given var
		// will be used by multiple constraints anyway. So, I'm not too
		// worried about this case at the moment.
		Symbol subject( chooseSubject( *rowptr ) );
		if( subject.type() == Symbol::Invalid )
			throw UnsatisfiableConstraint( constraint );

		rowptr->solveFor( subject );
		substitute( subject, *rowptr );
		subject.setBasic( true );
		m_rows[ subject ] = rowptr.release();
		m_cns[ constraint ] = tag;
	}

	/* Remove a constraint from the solver.

	If the constraint does not exist in the solver, this method is a
	no-op. This method always succeeds.

	*/
	void removeConstraint( const Constraint& constraint )
	{
		CnMap::iterator c_it = m_cns.find( constraint );
		if( c_it == m_cns.end() )
			return;

		CnTag tag( c_it->second );
		m_cns.erase( c_it );

		// Remove the error variables from the object *before* pivoting,
		// or substitutions into the objective will lead to bad results.
		if( tag.error1.type() != Symbol::Invalid )
			m_objective->remove( tag.error1 );
		if( tag.error2.type() != Symbol::Invalid )
			m_objective->remove( tag.error2 );

		// If the marker is basic, simply drop the row. Otherwise, pivot
		// the marker symbol into the basis and then drop the row.
		Symbol marker( tag.marker );
		RowMap::iterator r_it = m_rows.find( marker );
		if( r_it != m_rows.end() )
		{
			std::auto_ptr<Row> rowptr( r_it->second );
			m_rows.erase( r_it );
		}
		else
		{
			r_it = getMarkerLeavingRow( marker );
			Symbol leaving( r_it->first );
			std::auto_ptr<Row> rowptr( r_it->second );
			m_rows.erase( r_it );
			pivot( *rowptr, leaving, marker );
		}
	}

	/* Solve the system for the current set of constraints.

	This method will throw an UnboundedObjective exception if the set of
	constraints has an unbounded solution.

	*/
	void solve()
	{
		optimize();
		updateExternalVars();
	}

	void beginEdit()
	{
	}

	void endEdit()
	{
	}

	void suggestValue( const Variable& var, double value, double strength )
	{
	}

	/* Reset the solver to an empty starting condition.

	This can be faster than deleting the solver and creating a new one
	when all of the constraints need to be changed.

	*/
	void reset()
	{
		clearRows();
		m_cns.clear();
		m_vars.clear();
		m_objective.reset( new Row() );
		m_id_tick = 1;
	}

	void dump()
	{
		std::cout << "F: ";
		dump( *m_objective );
		std::cout << std::endl;
		dump( m_rows );
		std::cout << std::endl;
		dump( m_vars );
	}

	void dump( const RowMap& rows )
	{
		typedef RowMap::const_iterator iter_t;
		iter_t end = rows.end();
		for( iter_t it = m_rows.begin(); it != end; ++it )
		{
			dump( it->first );
			std::cout << " | ";
			dump( *it->second );
			std::cout << std::endl;
		}
	}

	void dump( const VarMap& vars )
	{
		typedef VarMap::const_iterator iter_t;
		iter_t end = m_vars.end();
		for( iter_t it = m_vars.begin(); it != end; ++it )
		{
			std::cout << it->first.name() << " = ";
			dump( it->second );
			std::cout << std::endl;
		}
	}

	void dump( const Row& row )
	{
		typedef Row::CellMap::const_iterator iter_t;
		std::cout << row.constant();
		iter_t end = row.cells().end();
		for( iter_t it = row.cells().begin(); it != end; ++it )
		{
			std::cout << " + " << it->second << " * ";
			dump( it->first );
		}
	}

	void dump( const Symbol& symbol )
	{
		switch( symbol.type() )
		{
			case Symbol::Invalid:
				std::cout << "i";
				break;
			case Symbol::External:
				std::cout << "v";
				break;
			case Symbol::Slack:
				std::cout << "s";
				break;
			case Symbol::Error:
				std::cout << "e";
				break;
			case Symbol::Dummy:
				std::cout << "d";
				break;
			default:
				break;
		}
		std::cout << symbol.id();
		if( symbol.isBasic() )
			std::cout << "!";
	}

private:

	SolverImpl( const SolverImpl& );

	SolverImpl& operator=( const SolverImpl& );

	struct RowDeleter
	{
		template<typename T>
		void operator()( T& pair )
		{
			delete pair.second;
		}
	};

	void clearRows()
	{
		std::for_each( m_rows.begin(), m_rows.end(), RowDeleter() );
		m_rows.clear();
	}

	/* Get the cached symbol for the given variable.

	If a symbol does not exist for the variable, one will be created.

	*/
	Symbol getSymbol( const Variable& variable )
	{
		VarMap::iterator it = m_vars.find( variable );
		if( it != m_vars.end() )
			return it->second;
		Symbol symbol( Symbol::External, m_id_tick++ );
		m_vars[ variable ] = symbol;
		return symbol;
	}

	/* Create a new row object for the given constraint.

	The terms in the constraint will be converted to cells in the row.
	Any term in the constraint with a coefficient of zero is ignored.
	This method uses the `getSymbol` method to get the symbol for the
	variables added to the row. If the symbol for a given variable is
	currently basic, it will be substituted with the basic row.

	The necessary slack and error variables will be added to the row.
	If the constant for the row is negative, the sign for the row
	will be inverted so the constant becomes positive. The tag will
	be updated with the symbol which can be used to track the effects
	of the constraint in the tableau.

	The CnTag arg will be updated with the marker and error symbols to
	use for tracking the movement of the constraint in the tableau.

	*/
	Row* createRow( const Constraint& constraint, CnTag& tag )
	{
		typedef std::vector<Term>::const_iterator iter_t;
		const Expression& expr( constraint.expression() );
		Row* row = new Row( expr.constant() );

		iter_t end = expr.terms().end();
		for( iter_t it = expr.terms().begin(); it != end; ++it )
		{
			if( !approx( it->coefficient(), 0.0 ) )
			{
				Symbol symbol( getSymbol( it->variable() ) );
				RowMap::const_iterator row_it = m_rows.find( symbol );
				if( row_it != m_rows.end() )
					row->insert( *row_it->second, it->coefficient() );
				else
					row->insert( symbol, it->coefficient() );
			}
		}

		switch( constraint.op() )
		{
			case OP_LE:
			case OP_GE:
			{
				double c = constraint.op() == OP_LE ? 1.0 : -1.0;
				Symbol slack( Symbol::Slack, m_id_tick++ );
				tag.marker = slack;
				row->insert( slack, c );
				if( constraint.strength() < strength::required )
				{
					Symbol error( Symbol::Error, m_id_tick++ );
					tag.error1 = error;
					row->insert( error, -c );
					m_objective->insert( error, constraint.strength() );
				}
				break;
			}
			case OP_EQ:
			{
				if( constraint.strength() < strength::required )
				{
					Symbol error1( Symbol::Error, m_id_tick++ );
					Symbol error2( Symbol::Error, m_id_tick++ );
					tag.marker = error1; // arbitrary choice
					tag.error1 = error1;
					tag.error2 = error2;
					row->insert( error1, 1.0 );
					row->insert( error2, -1.0 );
					m_objective->insert( error1, constraint.strength() );
					m_objective->insert( error2, constraint.strength() );
				}
				else
				{
					Symbol dummy( Symbol::Dummy, m_id_tick++ );
					tag.marker = dummy;
					row->insert( dummy );
				}
				break;
			}
		}

		if( row->constant() < 0.0 )
			row->reverseSign();

		return row;
	}

	/* Choose the subject for solving for the row.

	This method will choose the best subject for using as the solve
	target for the row. An invalid symbol will be returned if there
	is no valid target.

	The symbols are chosen according to the following precedence:

	1) The first symbol representing an external variable.
	2) The last negative slack or error variable.

	Rule #2 follows from the fact that variables which are new to the
	solver will always appear at the end of a row due to monotonically
	increasing symbol ids. This reduces the need for substitution.

	Assuming that the row has already had its basic symbols removed by
	substitution, the only time that a valid subject will not be found
	is when the constraint is required and cannot be satisfied. In that
	case, this method will return an invalid symbol.

	*/
	Symbol chooseSubject( const Row& row )
	{
		typedef Row::CellMap::const_iterator iter_t;
		Symbol result;
		iter_t end = row.cells().end();
		for( iter_t it = row.cells().begin(); it != end; ++it )
		{
			switch( it->first.type() )
			{
				case Symbol::External:
					return it->first;
				case Symbol::Slack:
				case Symbol::Error:
				{
					if( it->second < 0.0 )
						result = it->first;
					break;
				}
				default:
					break;
			}
		}
		return result;
	}

	/* Substitute the parametric symbol with the given row.

	This method will substitute all instances of the parametric symbol
	in the tableau and the objective function with the given row.

	*/
	void substitute( const Symbol& symbol, const Row& row )
	{
		typedef RowMap::iterator iter_t;
		iter_t end = m_rows.end();
		for( iter_t it = m_rows.begin(); it != end; ++it )
			it->second->substitute( symbol, row );
		m_objective->substitute( symbol, row );
	}

	/* Perform a pivot of the given row and symbols.

	This method will pivot the tableau with the given row so that the
	leaving symbol will leave the basis, and the entering symbol will
	enter the basis. The entering symbol *must* exist in the row, and
	the row *must not* currently be in the tableau. The caller is
	responsible for adding the row back to the tableau if needed.

	*/
	void pivot( Row& row, Symbol& leaving, Symbol& entering )
	{
		leaving.setBasic( false );
		entering.setBasic( true );
		row.solveFor( leaving, entering );
		substitute( entering, row );
	}

	/* Optimize the system for the current objective function.

	This method performs iterations of Phase 2 of the simplex method
	until the objective function reaches a minimum. If the objective
	function is unbounded, an UnboundedObjective exception is thrown.

	*/
	void optimize()
	{
		while( true )
		{
			Symbol entering( getEnteringSymbol() );
			if( entering.type() == Symbol::Invalid )
				return;

			RowMap::iterator it = getLeavingRow( entering );
			if( it == m_rows.end() )
				throw UnboundedObjective();

			Row* row = it->second;
			Symbol leaving( it->first );
			m_rows.erase( it );
			pivot( *row, leaving, entering );
			m_rows[ entering ] = row;
		}
	}

	/* Compute the entering variable for a pivot operation.

	This method will return first symbol in the objective function which
	is non-dummy, non-basic, and has a coefficient less than zero. If no
	no symbol meets the criteria, it means the objective function is at
	a minimum, and an invalid symbol is returned.

	*/
	Symbol getEnteringSymbol()
	{
		typedef Row::CellMap::const_iterator iter_t;
		iter_t begin = m_objective->cells().begin();
		iter_t end = m_objective->cells().end();
		for( iter_t it = begin; it != end; ++it )
		{
			if( it->first.type() != Symbol::Dummy &&
				!it->first.isBasic() &&
				it->second < 0.0 )
				return it->first;
		}
		return Symbol();
	}

	/* Compute the row which holds the exit symbol for a pivot.

	This method will return an iterator to the row in the row map
	which holds the exit symbol. If no appropriate exit symbol is
	found, the end() iterator will be returned. This indicates that
	the objective function is unbounded.

	*/
	RowMap::iterator getLeavingRow( const Symbol& entering )
	{
		typedef RowMap::iterator iter_t;
		double ratio = std::numeric_limits<double>::max();
		iter_t end = m_rows.end();
		iter_t found = m_rows.end();
		for( iter_t it = m_rows.begin(); it != end; ++it )
		{
			double temp = it->second->coefficientFor( entering );
			if( temp < 0.0 )
			{
				double temp_ratio = -it->second->constant() / temp;
				if( temp_ratio < ratio )
				{
					ratio = temp_ratio;
					found = it;
				}
			}
		}
		return found;
	}

	/* Compute the leaving row for a marker variable.

	This method will return an iterator to the row in the row map
	which holds the given marker variable. The row will be chosen
	according to the following precedence:

	1) The row with a restricted basic varible and a negative coefficient
	   for the marker with the smallest ratio of -constant / coefficient.

	2) The row with a restricted basic variable and the smallest ratio
	   of constant / coefficient.

	3) The last unrestricted row which contains the marker.

	The marker *must* exist in at least one row.

	*/
	RowMap::iterator getMarkerLeavingRow( const Symbol& marker )
	{
		const double dmax = std::numeric_limits<double>::max();
		typedef RowMap::iterator iter_t;
		iter_t begin = m_rows.begin();
		iter_t end = m_rows.end();
		iter_t first = m_rows.end();
		iter_t second = m_rows.end();
		iter_t third = m_rows.end();
		double r1 = dmax;
		double r2 = dmax;
		for( iter_t it = begin; it != end; ++it )
		{
			double c = it->second->coefficientFor( marker );
			if( c == 0.0 )
				continue;
			if( it->first.type() == Symbol::External )
			{
				third = it;
			}
			else if( c < 0.0 )
			{
				double r = -it->second->constant() / c;
				if( r < r1 )
				{
					r1 = r;
					first = it;
				}
			}
			else
			{
				double r = it->second->constant() / c;
				if( r < r2 )
				{
					r2 = r;
					second = it;
				}
			}
		}
		if( first != end )
			return first;
		if( second != end )
			return second;
		return third;
	}

	/* Update the external variables with the values from the tableau.

	*/
	void updateExternalVars()
	{
		typedef VarMap::iterator var_iter_t;
		typedef RowMap::iterator row_iter_t;
		var_iter_t var_begin = m_vars.begin();
		var_iter_t var_end = m_vars.end();
		row_iter_t row_end = m_rows.end();
		for( var_iter_t var_it = var_begin; var_it != var_end; ++var_it )
		{
			Variable& var( const_cast<Variable&>( var_it->first ) );
			row_iter_t row_it = m_rows.find( var_it->second );
			if( row_it == row_end )
				var.setValue( 0.0 );
			else
				var.setValue( row_it->second->constant() );
		}
	}

	CnMap m_cns;
	VarMap m_vars;
	RowMap m_rows;
	std::auto_ptr<Row> m_objective;
	Symbol::Id m_id_tick;
};

} // namespace impl

} // namespace kiwi