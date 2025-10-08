# Filters


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the comparison operator: &#x60;eq&#x60;, &#x60;ne&#x60;, &#x60;gt&#x60;, &#x60;gte&#x60;, &#x60;lt&#x60;, &#x60;lte&#x60;. - &#x60;eq&#x60;: equals - &#x60;ne&#x60;: not equal - &#x60;gt&#x60;: greater than - &#x60;gte&#x60;: greater than or equal - &#x60;lt&#x60;: less than - &#x60;lte&#x60;: less than or equal  | [default to 'eq']
**key** | **str** | The key to compare against the value. | 
**value** | [**ComparisonFilterValue**](ComparisonFilterValue.md) |  | 
**filters** | [**List[CompoundFilterFiltersInner]**](CompoundFilterFiltersInner.md) | Array of filters to combine. Items can be &#x60;ComparisonFilter&#x60; or &#x60;CompoundFilter&#x60;. | 

## Example

```python
from openapi_client.models.filters import Filters

# TODO update the JSON string below
json = "{}"
# create an instance of Filters from a JSON string
filters_instance = Filters.from_json(json)
# print the JSON string representation of the object
print(Filters.to_json())

# convert the object into a dict
filters_dict = filters_instance.to_dict()
# create an instance of Filters from a dict
filters_from_dict = Filters.from_dict(filters_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


